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
    // Link the full optional-tiers lib (libasmtest_emu_full: Keystone + Capstone)
    // and enable the in-line-assembler AND disassembler tests. `make zig-asm-test`
    // passes -Dasm=true; the default build links the plain libasmtest_emu and
    // compiles those cases out.
    const with_asm = b.option(bool, "asm", "link the optional native tiers (Keystone + Capstone)") orelse false;

    const tests = b.addTest(.{
        .root_source_file = b.path("src/conformance.zig"),
        .target = target,
        .optimize = optimize,
    });
    tests.linkLibC();
    tests.addIncludePath(.{ .cwd_relative = incdir });
    tests.addLibraryPath(.{ .cwd_relative = libdir });
    tests.linkSystemLibrary(if (with_asm) "asmtest_emu_full" else "asmtest_emu");
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
