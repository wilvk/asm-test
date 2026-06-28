//! Tests for the in-process DynamoRIO native-trace wrapper (asmtest::drtrace).
//!
//! Self-skips unless the tier is built AND DynamoRIO is resolvable — i.e. unless
//! ASMTEST_DRAPP_LIB (and ASMTEST_DRCLIENT / ASMTEST_DR_LIB or DYNAMORIO_HOME)
//! point at a built libasmtest_drapp + client on a DynamoRIO-capable Linux
//! x86-64 host. The `make docker-drtrace` lane sets these up in a container; on a
//! dev box build with `make shared-drtrace drtrace-client DYNAMORIO_HOME=...` and
//! export the env. When DynamoRIO is absent the test prints SKIP and returns —
//! it does NOT panic/fail.

use asmtest::drtrace::{NativeCode, NativeTrace};

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE: [u8; 18] = [
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48,
    0xFF, 0xC8, 0xC3,
];

#[test]
fn native_trace_coverage_and_instruction_mode() {
    if !NativeTrace::available() {
        eprintln!("SKIP: DynamoRIO native-trace tier unavailable (self-skip)");
        return;
    }
    if std::env::var("ASMTEST_DRCLIENT").map_or(true, |v| v.is_empty()) {
        eprintln!("SKIP: ASMTEST_DRCLIENT not set (build the DR client)");
        return;
    }
    if let Err(e) = NativeTrace::initialize_default() {
        eprintln!("SKIP: dr_init/start failed: {e}");
        return;
    }

    // ---- block coverage + accumulation ----
    let code = NativeCode::from_bytes(&ROUTINE);
    let tr = NativeTrace::new_trace(64, 0);
    tr.register("add2", &code).expect("register add2");

    {
        let _r = tr.region("add2");
        assert_eq!(code.call2(20, 22), 42);
    }
    assert!(tr.covered(0)); // entry block

    let before = tr.blocks_len();
    {
        let _r = tr.region("add2");
        // 120 > 100 -> dec -> 119, takes the other block.
        assert_eq!(code.call2(60, 60), 119);
    }
    assert!(tr.blocks_len() >= before);
    assert_eq!(NativeTrace::marker_error(), 0);

    tr.unregister("add2");
    drop(code);
    drop(tr);

    // ---- instruction mode ----
    let code2 = NativeCode::from_bytes(&ROUTINE);
    let tr2 = NativeTrace::new_trace(64, 64);
    tr2.register("add2i", &code2).expect("register add2i");
    {
        let _r = tr2.region("add2i");
        assert_eq!(code2.call2(1, 2), 3);
    }
    assert!(tr2.insns_total() >= 4); // ordered instruction stream recorded
    tr2.unregister("add2i");
    drop(code2);
    drop(tr2);

    NativeTrace::shutdown();
}
