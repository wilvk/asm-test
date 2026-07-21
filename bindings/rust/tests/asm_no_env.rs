//! The in-line assembler/disassembler must be reachable WITHOUT `ASMTEST_LIB`
//! (2026-07-21 review B1): the crate dylib-links libasmtest_emu — which carries
//! Keystone + Capstone — so `asm_fns()` falls back to `dlopen(NULL)` and
//! resolves the symbols from the already-linked image. Pre-fix, the gate was
//! the env var alone and the common no-env downstream case reported the tiers
//! "not in this build" despite the linked-in symbols.
//!
//! This is its OWN integration-test binary (= its own process) on purpose: the
//! function pointers are resolved once per process, so `ASMTEST_LIB` must be
//! gone before anything touches the tier — impossible to guarantee inside the
//! shared conformance binary, whose harness environment sets the variable.

use asmtest::{AsmArch, AsmSyntax};

#[test]
fn asm_tier_reachable_without_env_override() {
    std::env::remove_var("ASMTEST_LIB");

    assert!(
        asmtest::asm_available(),
        "asm tier must resolve via dlopen(NULL) on the linked libasmtest_emu \
         (no ASMTEST_LIB set)"
    );
    assert!(
        asmtest::disas_available(),
        "disas tier must resolve via dlopen(NULL) on the linked libasmtest_emu \
         (no ASMTEST_LIB set)"
    );

    // Prove the fallback handle actually WORKS, not merely that the pointers
    // are non-null: assemble an AArch64 `ret` (host-arch independent) and
    // disassemble it back.
    let a64 = asmtest::assemble("ret", AsmArch::Arm64, AsmSyntax::Intel, 0x0010_0000)
        .expect("assemble arm64 through the dlopen(NULL) fallback");
    assert_eq!(a64, vec![0xC0, 0x03, 0x5F, 0xD6]);

    let listing = asmtest::disas(&a64, 0, AsmArch::Arm64, 0x0010_0000);
    assert!(
        listing.contains("ret"),
        "disas through the fallback should render the ret (got: {listing:?})"
    );
}
