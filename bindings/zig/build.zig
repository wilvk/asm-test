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

    // DynamoRIO native-trace smoke test (Track Z). A SEPARATE step so it does
    // NOT break `zig build test`: it must not link libasmtest_drapp at build
    // time (that lib needs DynamoRIO and may be absent). It dlopen()s the lib at
    // runtime via std.DynLib and self-skips when it can't load. It still needs
    // the include dir so `@cImport("asmtest_drtrace.h")` can translate the
    // option/exec-code struct *types* (and the asmtest_trace.h it includes), and
    // libC for std.DynLib / posix. No addLibraryPath / linkSystemLibrary.
    const drtrace_exe = b.addExecutable(.{
        .name = "drtrace-test",
        .root_source_file = b.path("src/drtrace_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    drtrace_exe.linkLibC();
    drtrace_exe.addIncludePath(.{ .cwd_relative = incdir });

    const run_drtrace = b.addRunArtifact(drtrace_exe);
    // Forward the parent environment (ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT /
    // DYNAMORIO_HOME / ASMTEST_DR_LIB) so the runtime dlopen + DR config see it.
    if (b.args) |args| run_drtrace.addArgs(args);
    const drtrace_step = b.step("drtrace-test", "Run the DynamoRIO native-trace smoke test (self-skips if unavailable)");
    drtrace_step.dependOn(&run_drtrace.step);

    // Hardware-trace (single-step / Intel PT / AMD) live test. A SEPARATE step
    // for the same reason as drtrace-test: it must NOT link libasmtest_hwtrace at
    // build time (that lib is optional and may be absent). It dlopen()s the lib at
    // runtime via std.DynLib and self-skips when it can't load (or the backend is
    // unavailable). It still needs the include dir so `@cImport("asmtest_hwtrace.h")`
    // can translate the option struct *type* (and the asmtest_trace.h it includes),
    // and libC for std.DynLib / posix. No addLibraryPath / linkSystemLibrary.
    const hwtrace_exe = b.addExecutable(.{
        .name = "hwtrace-test",
        .root_source_file = b.path("src/hwtrace_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    hwtrace_exe.linkLibC();
    hwtrace_exe.addIncludePath(.{ .cwd_relative = incdir });

    const run_hwtrace = b.addRunArtifact(hwtrace_exe);
    // Forward the parent environment (ASMTEST_HWTRACE_LIB) so the runtime dlopen
    // sees it.
    if (b.args) |args| run_hwtrace.addArgs(args);
    const hwtrace_step = b.step("hwtrace-test", "Run the single-step hardware-trace live test (self-skips if unavailable)");
    hwtrace_step.dependOn(&run_hwtrace.step);
}
