//! Locate and link the asm-test shared libraries.
//!
//! The binding has no crates.io dependencies; it links the prebuilt shared
//! objects directly. The directory is `ASMTEST_LIB_DIR` if set, else the repo's
//! `build/` dir relative to this crate. `make rust-test` builds the libs there
//! first and sets `ASMTEST_LIB_DIR` / `LD_LIBRARY_PATH` for the test run.

use std::env;

fn main() {
    let dir = env::var("ASMTEST_LIB_DIR").unwrap_or_else(|_| {
        let manifest = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
        format!("{manifest}/../../build")
    });

    println!("cargo:rustc-link-search=native={dir}");
    // libasmtest_emu: capture trampoline + verdict shims + emulator (superset).
    println!("cargo:rustc-link-lib=dylib=asmtest_emu");
    // The canonical routines under test, as a fixture lib (make's CORPUS_LIB).
    println!("cargo:rustc-link-lib=dylib=asmtest_corpus");
    // Find the .so at run time without an install step.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");

    println!("cargo:rerun-if-env-changed=ASMTEST_LIB_DIR");
    println!("cargo:rerun-if-changed=build.rs");
}
