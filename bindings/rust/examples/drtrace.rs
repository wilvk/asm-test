//! Single-threaded native-trace (DynamoRIO) wrapper check for the Rust binding.
//!
//! This runs as an EXAMPLE binary (a plain `fn main` on the process's main thread)
//! rather than a `#[test]`, because `cargo test` runs each test on a spawned worker
//! thread — so the process is multi-threaded when `dr_app_start` takes over, which
//! DynamoRIO cannot do reliably in-process (it crashes). A single-threaded `main`
//! lets DR take over cleanly, matching the C++/Zig binaries. Run via
//! `make drtrace-rust-test` (or `cargo run --example drtrace`).
//!
//! Self-skips (exit 0) unless the tier is built AND DynamoRIO is resolvable.

use asmtest::drtrace::{NativeCode, NativeTrace};
use std::process::exit;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE: [u8; 18] = [
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48,
    0xFF, 0xC8, 0xC3,
];

fn main() {
    if !NativeTrace::available() {
        eprintln!("# SKIP: DynamoRIO native-trace tier unavailable (self-skip)");
        return;
    }
    if std::env::var("ASMTEST_DRCLIENT").map_or(true, |v| v.is_empty()) {
        eprintln!("# SKIP: ASMTEST_DRCLIENT not set (build the DR client)");
        return;
    }
    if let Err(e) = NativeTrace::initialize_default() {
        eprintln!("# SKIP: dr_init/start failed: {e}");
        return;
    }

    // ---- block coverage + accumulation ----
    let code = NativeCode::from_bytes(&ROUTINE);
    let tr = NativeTrace::new_trace(64, 0);
    tr.register("add2", &code).expect("register add2");
    {
        let _r = tr.region("add2");
        assert_eq!(code.call2(20, 22), 42, "traced call returns 20+22");
    }
    assert!(tr.covered(0), "entry block covered");

    let before = tr.blocks_len();
    {
        let _r = tr.region("add2");
        assert_eq!(code.call2(60, 60), 119, "second call takes the dec branch");
    }
    assert!(tr.blocks_len() >= before, "coverage accumulates");
    assert_eq!(NativeTrace::marker_error(), 0, "markers balanced");
    tr.unregister("add2");
    drop(code);
    drop(tr);

    // ---- instruction mode ----
    let code2 = NativeCode::from_bytes(&ROUTINE);
    let tr2 = NativeTrace::new_trace(64, 64);
    tr2.register("add2i", &code2).expect("register add2i");
    {
        let _r = tr2.region("add2i");
        assert_eq!(code2.call2(1, 2), 3, "instruction-mode routine computes");
    }
    assert!(tr2.insns_total() >= 4, "ordered instruction stream recorded");
    assert_eq!(
        tr2.insn_offsets(),
        vec![0x0, 0x3, 0x6, 0xc, 0x11],
        "instruction-offset stream matches the executed (1,2) path"
    );
    assert!(
        tr2.block_offsets().contains(&0),
        "block offsets include the entry block"
    );
    tr2.unregister("add2i");
    drop(code2);
    drop(tr2);

    NativeTrace::shutdown();
    println!("PASS");
    let _ = exit; // (exit imported for symmetry with other bindings; success path returns)
}
