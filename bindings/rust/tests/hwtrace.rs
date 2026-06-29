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

use asmtest::hwtrace::{Backend, HwTrace, NativeCode};

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
