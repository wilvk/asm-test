//! The Rust native-trace (DynamoRIO) wrapper check lives in `examples/drtrace.rs`,
//! NOT here. `cargo test` runs each `#[test]` on a spawned worker thread, so the
//! process is multi-threaded when `dr_app_start` takes over — which DynamoRIO
//! cannot do reliably in-process (it crashes). The real check therefore runs as a
//! single-threaded example binary (`fn main` on the main thread):
//!
//!     make drtrace-rust-test          # or: cargo run --example drtrace
//!
//! This stub keeps `cargo test` safe and green (it never starts DynamoRIO).

#[test]
fn native_trace_runs_as_an_example_not_a_test() {
    // Intentionally does not initialize DynamoRIO — see the module docs and
    // examples/drtrace.rs for the actual wrapper validation.
    eprintln!("native-trace wrapper test runs via `cargo run --example drtrace` \
               (single-threaded); see examples/drtrace.rs");
}
