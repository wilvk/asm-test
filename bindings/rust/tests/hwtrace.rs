//! Live test for the single-step hardware-trace backend via the Rust wrapper.
//!
//! Unlike the DynamoRIO wrapper (which needs a DynamoRIO install AND a
//! single-threaded `fn main` — so it runs as `examples/drtrace.rs`, not a test), the
//! SINGLESTEP backend has NO thread-takeover constraint: it drives EFLAGS.TF
//! `#DB`/SIGTRAP entirely within the calling thread. It therefore runs as a normal
//! `cargo test` integration test here and asserts a real, live trace — on any
//! x86-64 Linux host, in CI, and in containers — self-skipping (with a printed note)
//! only where the backend is unavailable.
//!
//! Run: `ASMTEST_HWTRACE_LIB=<repo>/build/libasmtest_hwtrace.so \
//!       cargo test --test hwtrace -- --nocapture`

use asmtest::hwtrace::{
    Backend, Fidelity, HwTrace, NativeCode, Policy, Ptrace, Tier, TracePolicy,
    ASMTEST_HW_EUNAVAIL,
};

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret   (two basic blocks)
const ROUTINE: [u8; 18] = [
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48,
    0xFF, 0xC8, 0xC3,
];

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
const LOOP: [u8; 16] = [
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3,
];

#[test]
fn singlestep_live_trace() {
    if !HwTrace::available(Backend::Singlestep) {
        eprintln!(
            "# SKIP: single-step hardware-trace backend unavailable: {}",
            HwTrace::skip_reason(Backend::Singlestep)
        );
        return;
    }
    HwTrace::init(Backend::Singlestep).expect("hwtrace init (single-step)");

    // ---- routine: two blocks, the jle-taken / dec-skipped path ----
    {
        let code = NativeCode::from_bytes(&ROUTINE);
        let tr = HwTrace::new_trace(64, 64);
        tr.register("add2", &code).expect("register add2");

        let result = {
            let _r = tr.region("add2");
            code.call(20, 22) // 42 <= 100 -> jle taken, dec skipped
        };

        assert_eq!(result, 42, "traced call returns 20+22");
        // Byte-for-byte the Unicorn/DynamoRIO/PT/AMD/Python result for this fixture.
        assert_eq!(
            tr.insn_offsets(),
            vec![0x0, 0x3, 0x6, 0xC, 0x11],
            "instruction-offset stream matches the executed (20,22) path"
        );
        assert_eq!(tr.insns_total(), 5, "five instructions executed");
        assert!(tr.covered(0) && tr.covered(0x11), "entry + ret blocks covered");
        assert_eq!(tr.blocks_len(), 2, "two basic blocks");
        assert!(!tr.truncated(), "stream not truncated");
    }

    // ---- loop: 20 iterations, exact and complete (no depth ceiling) ----
    {
        let code = NativeCode::from_bytes(&LOOP);
        // instructions=256 so all 62 loop instructions are stored without truncation.
        let tr = HwTrace::new_trace(64, 256);
        tr.register("loop", &code).expect("register loop");

        let result = {
            let _r = tr.region("loop");
            code.call(1, 20)
        };

        assert_eq!(result, 20, "loop accumulates 1 twenty times");
        assert_eq!(tr.insns_total(), 62, "1 + 20*3 + 1, all captured");
        assert!(tr.covered(0) && tr.covered(0x7), "entry + loop-body blocks covered");
        assert_eq!(tr.blocks_len(), 2, "two basic blocks");
        assert!(!tr.truncated(), "stream not truncated");
    }

    HwTrace::shutdown();
    eprintln!("# PASS: single-step hardware-trace wrapper (routine + loop)");
}

#[test]
fn auto_resolve_selection_invariants() {
    // The orchestrator's selection invariants hold on every host (even where all
    // backends self-skip and the cascade is empty).
    let best = HwTrace::resolve(Policy::Best);
    let cf = HwTrace::resolve(Policy::CeilingFree);

    // Every resolved backend is actually available, ordered by descending fidelity
    // (ascending enum), with no duplicates.
    assert!(
        best.iter().all(|&b| HwTrace::available(backend_of(b))),
        "resolve(BEST) returns only available backends"
    );
    let mut sorted_deduped = best.clone();
    sorted_deduped.sort_unstable();
    sorted_deduped.dedup();
    assert_eq!(best, sorted_deduped, "resolve(BEST) is ascending enum order, no dups");

    // CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
    // subset of BEST.
    assert!(
        !cf.contains(&(Backend::AmdLbr as i32)),
        "resolve(CEILING_FREE) never selects AMD LBR"
    );
    assert!(
        cf.iter().all(|b| best.contains(b)),
        "resolve(CEILING_FREE) is a subset of resolve(BEST)"
    );

    // auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
    let ab = HwTrace::auto(Policy::Best);
    let expect = best.first().copied().unwrap_or(ASMTEST_HW_EUNAVAIL);
    assert_eq!(ab, expect, "auto(BEST) is the head of the resolved cascade");
}

#[test]
fn cross_tier_resolve_invariants() {
    // The cross-tier orchestrator (resolve over hwtrace + DynamoRIO + emulator)
    // holds its structural invariants on every host.
    let best = HwTrace::resolve_tiers(TracePolicy::Best);
    let nat = HwTrace::resolve_tiers(TracePolicy::NativeOnly);
    let cf = HwTrace::resolve_tiers(TracePolicy::CeilingFree);

    // Skip cleanly where the lib is absent (resolve_tiers returns empty) — there is
    // no cross-tier cascade to assert on, exactly like the live-trace self-skip.
    if best.is_empty() {
        eprintln!(
            "# SKIP: cross-tier cascade empty (libasmtest_hwtrace not loaded): {}",
            HwTrace::skip_reason(Backend::Singlestep)
        );
        return;
    }

    // Every HW choice satisfies the hardware-tier probe; NATIVE choices precede the
    // single VIRTUAL emulator floor, which is the last entry under BEST.
    for c in &best {
        if c.tier == Tier::HwTrace {
            assert!(
                HwTrace::available(backend_of(c.backend)),
                "every HW-tier choice is an available backend"
            );
        }
        let want = if c.tier == Tier::Emulator { Fidelity::Virtual } else { Fidelity::Native };
        assert_eq!(c.fidelity, want, "fidelity matches tier (emulator => virtual)");
    }
    assert_eq!(
        best.last().map(|c| c.tier),
        Some(Tier::Emulator),
        "the emulator floor is the last BEST entry"
    );
    assert_eq!(
        best.iter().filter(|c| c.tier == Tier::Emulator).count(),
        1,
        "exactly one emulator floor entry under BEST"
    );

    // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
    assert!(
        nat.iter().all(|c| c.tier != Tier::Emulator),
        "resolve(NATIVE_ONLY) never includes the emulator tier"
    );
    assert_eq!(nat.len(), best.len() - 1, "NATIVE_ONLY is BEST minus the emulator floor");

    // CEILING_FREE drops AMD LBR.
    assert!(
        cf.iter()
            .all(|c| !(c.tier == Tier::HwTrace && c.backend == Backend::AmdLbr as i32)),
        "resolve(CEILING_FREE) never selects the AMD LBR backend"
    );

    // auto(policy) is the head of resolve(policy).
    let one = HwTrace::auto_tier(TracePolicy::Best).expect("auto_tier(BEST) resolves a choice");
    assert_eq!(
        (one.tier, one.backend),
        (best[0].tier, best[0].backend),
        "auto_tier(BEST) is the head of the resolved cross-tier cascade"
    );
}

#[test]
fn cross_tier_native_only_resolves_on_linux_x86_64() {
    // On any x86-64 Linux host the single-step backend is a native floor, so even
    // NATIVE_ONLY resolves (the cascade never collapses to nothing here). Guard
    // exactly like singlestep_live_trace so we self-skip off the single-step floor.
    if !HwTrace::available(Backend::Singlestep) {
        eprintln!(
            "# SKIP: single-step hardware-trace backend unavailable: {}",
            HwTrace::skip_reason(Backend::Singlestep)
        );
        return;
    }

    let nat = HwTrace::resolve_tiers(TracePolicy::NativeOnly);
    let pick = HwTrace::auto_tier(TracePolicy::NativeOnly);

    assert!(!nat.is_empty(), "NATIVE_ONLY resolves a non-empty cascade on x86-64 Linux");
    let pick = pick.expect("auto_tier(NATIVE_ONLY) resolves a native choice here");
    assert_eq!(pick.fidelity, Fidelity::Native, "the NATIVE_ONLY pick is a native choice");
    assert!(
        nat.iter()
            .any(|c| c.tier == Tier::HwTrace && c.backend == Backend::Singlestep as i32),
        "the single-step backend is present as a native floor under NATIVE_ONLY"
    );
    eprintln!("# PASS: cross-tier NATIVE_ONLY resolves on x86-64 Linux");
}

#[test]
fn auto_resolve_traces_live() {
    // On any x86-64 Linux host the cascade is non-empty (single-step floor), so
    // auto() resolves a usable backend; trace the shared fixture through it. Guard
    // exactly like singlestep_live_trace so we self-skip off the single-step floor.
    if !HwTrace::available(Backend::Singlestep) {
        eprintln!(
            "# SKIP: single-step hardware-trace backend unavailable: {}",
            HwTrace::skip_reason(Backend::Singlestep)
        );
        return;
    }

    let best = HwTrace::resolve(Policy::Best);
    let ab = HwTrace::auto(Policy::Best);
    assert!(!best.is_empty() && ab >= 0, "single-step keeps the cascade non-empty here");

    // The tier is a single global lifecycle, so this section owns its own
    // init/shutdown rather than nesting inside another active region.
    HwTrace::init(backend_of(ab)).expect("hwtrace init (auto)");
    {
        let code = NativeCode::from_bytes(&ROUTINE);
        let tr = HwTrace::new_trace(64, 64);
        tr.register("auto", &code).expect("register auto");

        let result = {
            let _r = tr.region("auto");
            code.call(20, 22)
        };

        assert_eq!(result, 42, "auto-selected backend traces a live call");
        assert!(tr.covered(0), "auto-selected backend covers block offset 0");
        if ab == Backend::Singlestep as i32 {
            // The pick off PT/AMD hosts: byte-exact parity with the shared fixture.
            assert_eq!(
                tr.insn_offsets(),
                vec![0x0, 0x3, 0x6, 0xC, 0x11],
                "auto pick (single-step) yields the exact shared offset stream"
            );
        }
    }
    HwTrace::shutdown();
    eprintln!("# PASS: auto-selected hardware-trace wrapper (live trace)");
}

/// Map a resolved backend enum int (from [`HwTrace::resolve`] / [`HwTrace::auto`])
/// back to a [`Backend`] for the enum-typed wrapper entries (`available`, `init`).
fn backend_of(b: i32) -> Backend {
    match b {
        0 => Backend::IntelPt,
        1 => Backend::CoreSight,
        2 => Backend::AmdLbr,
        3 => Backend::Singlestep,
        other => panic!("unexpected backend enum {other}"),
    }
}

// ---- Out-of-process / foreign-process toolkit (asmtest::hwtrace::Ptrace) ----
//
// The ptrace toolkit ships in the same libasmtest_hwtrace the wrapper above
// loads. Each test self-skips (with a printed note) when the backend is absent,
// matching the live-trace self-skip above. `trace_attached` and `run_to` have no
// live test (forking + ptrace of a foreign process is impractical; the C suite
// covers them live), but both symbols ARE wrapped in src/hwtrace.rs so the
// binding-parity gate is satisfied, and `run_to`'s FFI round-trip is probed below.

/// True (printing a skip note) when the ptrace backend is unavailable, so each
/// ptrace test can `return` early exactly like `singlestep_live_trace`.
fn skip_if_no_ptrace() -> bool {
    if !Ptrace::available() {
        eprintln!("# SKIP: ptrace backend unavailable: {}", Ptrace::skip_reason());
        return true;
    }
    false
}

#[test]
fn ptrace_trace_call() {
    // Fork a tracee, single-step it OUT OF PROCESS, get the same offsets.
    if skip_if_no_ptrace() {
        return;
    }
    let code = NativeCode::from_bytes(&ROUTINE);
    let tr = HwTrace::new_trace(64, 64);

    let result = Ptrace::trace_call(&code, &[20, 22], &tr);

    assert_eq!(result, 42, "out-of-process traced call returns 20+22");
    assert_eq!(
        tr.insn_offsets(),
        vec![0x0, 0x3, 0x6, 0xC, 0x11],
        "out-of-process offsets match the in-process / Python stream"
    );
    assert!(!tr.truncated(), "stream not truncated");
    eprintln!("# PASS: Ptrace::trace_call (out-of-process single-step)");
}

#[test]
fn ptrace_run_to_rejects_null() {
    // run_to drives an attached target to a resolved method (software breakpoint). A
    // live foreign attach is covered by the C suite; here exercise the FFI round-trip
    // safely — a NULL target address is rejected (EINVAL, non-OK) before any ptrace call.
    if skip_if_no_ptrace() {
        return;
    }
    let rc = Ptrace::run_to(std::process::id() as i32, 0);
    assert_ne!(rc, 0, "run_to(NULL addr) is rejected (EINVAL) via the FFI round-trip");
    eprintln!("# PASS: Ptrace::run_to FFI round-trip (NULL addr -> EINVAL)");
}

#[test]
fn proc_region_by_addr() {
    // Discover an executable region's extent from /proc/<pid>/maps by an interior
    // address (this process).
    if skip_if_no_ptrace() {
        return;
    }
    let code = NativeCode::from_bytes(&ROUTINE);
    let pid = std::process::id() as i32;

    let region = Ptrace::region_by_addr(pid, code.base() + 4);
    let (base, len) = region.expect("an executable mapping contains an interior address");
    assert_eq!(base, code.base(), "region base is the mapping start");
    assert!(len >= ROUTINE.len(), "region spans at least the routine bytes");

    // Nothing maps address 1.
    assert!(
        Ptrace::region_by_addr(pid, 0x1).is_none(),
        "no executable mapping contains addr 1"
    );
    eprintln!("# PASS: Ptrace::region_by_addr (/proc/<pid>/maps resolve)");
}

#[test]
fn proc_perfmap_symbol() {
    // Parse a JIT perf-map (/tmp/perf-<pid>.map) and resolve a method by name.
    if skip_if_no_ptrace() {
        return;
    }
    let pid = std::process::id() as i32;
    let path = format!("/tmp/perf-{pid}.map");
    std::fs::write(&path, "400000 1a void demo(long, long)\n500000 8 other\n")
        .expect("write perf map");

    let found = Ptrace::perfmap_symbol(pid, "void demo(long, long)");
    let missing = Ptrace::perfmap_symbol(pid, "missing");
    let _ = std::fs::remove_file(&path);

    assert_eq!(found, Some((0x400000, 0x1A)), "resolve a perf-map symbol by name");
    assert!(missing.is_none(), "an absent perf-map symbol resolves to None");
    eprintln!("# PASS: Ptrace::perfmap_symbol (perf-map resolve)");
}

#[test]
fn jitdump_find() {
    // Read a binary jitdump and resolve a method to (addr,size,index,ts) + bytes.
    if skip_if_no_ptrace() {
        return;
    }
    let mut dir = std::env::temp_dir();
    dir.push(format!("asmtest-jitdump-{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("create jitdump dir");
    let path = dir.join("jit.dump");
    let name: &[u8] = b"void demo(long, long)";

    // Little-endian binary jitdump, mirroring the Python test's struct.pack layout.
    let mut buf: Vec<u8> = Vec::new();
    // header: magic, version, total_size=40, elf_mach, pad1 (all u32), then
    // pid, timestamp, flags (u64) — "<IIIIIIQQ".
    buf.extend_from_slice(&0x4A69_5444u32.to_le_bytes()); // magic
    buf.extend_from_slice(&1u32.to_le_bytes()); // version
    buf.extend_from_slice(&40u32.to_le_bytes()); // total_size (header)
    buf.extend_from_slice(&62u32.to_le_bytes()); // elf_mach
    buf.extend_from_slice(&0u32.to_le_bytes()); // pad1
    buf.extend_from_slice(&0u32.to_le_bytes()); // pid
    buf.extend_from_slice(&0u64.to_le_bytes()); // timestamp
    buf.extend_from_slice(&0u64.to_le_bytes()); // flags
    // JIT_CODE_LOAD record header: id=0, total_size, timestamp=5 — "<IIQ".
    let total: u64 = 16 + 40 + (name.len() as u64 + 1) + ROUTINE.len() as u64;
    buf.extend_from_slice(&0u32.to_le_bytes()); // id (JIT_CODE_LOAD)
    buf.extend_from_slice(&(total as u32).to_le_bytes()); // total_size
    buf.extend_from_slice(&5u64.to_le_bytes()); // timestamp
    // body: pid, tid (u32), vma, code_addr, code_size, code_index (u64) — "<IIQQQQ".
    buf.extend_from_slice(&0u32.to_le_bytes()); // pid
    buf.extend_from_slice(&0u32.to_le_bytes()); // tid
    buf.extend_from_slice(&0x2000u64.to_le_bytes()); // vma
    buf.extend_from_slice(&0x2000u64.to_le_bytes()); // code_addr
    buf.extend_from_slice(&(ROUTINE.len() as u64).to_le_bytes()); // code_size
    buf.extend_from_slice(&9u64.to_le_bytes()); // code_index
    buf.extend_from_slice(name);
    buf.push(0); // NUL-terminated symbol name
    buf.extend_from_slice(&ROUTINE);
    std::fs::write(&path, &buf).expect("write jitdump");

    let path_str = path.to_str().expect("utf-8 jitdump path");
    let found = Ptrace::jitdump_find(Some(path_str), "void demo(long, long)", 0, 64);
    let missing = Ptrace::jitdump_find(Some(path_str), "missing", 0, 0);
    let _ = std::fs::remove_dir_all(&dir);

    let m = found.expect("resolve a jitdump method by name");
    assert_eq!(
        (m.code_addr, m.code_size, m.code_index, m.timestamp),
        (0x2000, ROUTINE.len() as u64, 9, 5),
        "jitdump method resolves to the recorded (addr,size,index,ts)"
    );
    assert_eq!(m.code, ROUTINE.to_vec(), "jitdump copies the recorded code bytes");
    assert!(missing.is_none(), "an absent jitdump method resolves to None");
    eprintln!("# PASS: Ptrace::jitdump_find (binary jitdump resolve + bytes)");
}
