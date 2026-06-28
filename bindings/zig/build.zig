//! Build script for the asm-test Zig binding (targets Zig 0.13.x).
//!
//! `zig build test` compiles and runs the conformance tests, linking the
//! prebuilt shared libraries. Override the locations with
//! `-Dincdir=<asm-test include dir>` and `-Dlibdir=<asm-test build dir>`
//! (defaults assume this crate sits at bindings/zig in the repo). `make
//! zig-test` passes absolute paths.
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const incdir = b.option([]const u8, "incdir", "asm-test include dir") orelse
        "../../include";
    const libdir = b.option([]const u8, "libdir", "asm-test build dir") orelse
        "../../build";
    // libasmtest_emu is the full superset (emulator + Keystone assembler +
    // Capstone disassembler), so the in-line-assembler AND disassembler tests are
    // enabled by default — no flag. -Dasm=false can still compile those cases out
    // (e.g. when linking against an older lean lib).
    const with_asm = b.option(bool, "asm", "link the optional native tiers (Keystone + Capstone)") orelse true;

    const tests = b.addTest(.{
        .root_source_file = b.path("src/conformance.zig"),
        .target = target,
        .optimize = optimize,
    });
    tests.linkLibC();
    tests.addIncludePath(.{ .cwd_relative = incdir });
    tests.addLibraryPath(.{ .cwd_relative = libdir });
    tests.linkSystemLibrary("asmtest_emu");
    tests.linkSystemLibrary("asmtest_corpus");
    tests.addRPath(.{ .cwd_relative = libdir });

    // Surface the toggle to the test source so the asm cases compile only when
    // the assembler lib is linked (otherwise its symbols would be undefined).
    const opts = b.addOptions();
    opts.addOption(bool, "with_asm", with_asm);
    tests.root_module.addOptions("build_options", opts);

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the conformance tests");
    test_step.dependOn(&run_tests.step);
}
