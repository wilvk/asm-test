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
    // The optional in-line assembler is resolved with the libc dynamic loader
    // (dlopen/dlsym); on Linux that needs -ldl (a no-op stub on glibc >= 2.34,
    // and unneeded on macOS where dl lives in libSystem).
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-lib=dylib=dl");
    }
    // Find the .so at run time without an install step.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");

    println!("cargo:rerun-if-env-changed=ASMTEST_LIB_DIR");
    println!("cargo:rerun-if-changed=build.rs");
}
