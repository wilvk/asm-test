// Zig data-flow binding smoke (Phase 6 + F7): GC-move canonicalizer + method
// resolver, mirroring the Python/C++/Node/Ruby/Lua suites — and (F7) a REAL live
// attach to a victim process by pid. dlopen's libasmtest_dataflow via std.DynLib
// (like hwtrace.zig). Run:
//   zig run -lc bindings/zig/src/test_dataflow.zig  (ASMTEST_DATAFLOW_LIB +
//                                                    ASMTEST_DATAFLOW_VICTIM set)
const std = @import("std");

const GcMove = extern struct { old_base: u64, new_base: u64, len: u64, step: u32 };
const Method = extern struct { addr: u64, size: u64, name: [*:0]const u8, version: u64 };
const GcmoveCanonFn = *const fn (?[*]const GcMove, usize, u32, u64) callconv(.C) u64;
const MethodResolveFn = *const fn (?[*]const Method, usize, u64) callconv(.C) c_int;

// The L0 sink handle is opaque — passed around, never inspected.
const ValtraceNewFn = *const fn (usize, usize, usize) callconv(.C) ?*anyopaque;
const ValtraceFreeFn = *const fn (?*anyopaque) callconv(.C) void;
const ValtraceStepsFn = *const fn (?*anyopaque) callconv(.C) usize;
const ValtraceRecsFn = *const fn (?*anyopaque) callconv(.C) usize;

// F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c). The producer
// ships NO header on purpose (a value-trace PRODUCER is a tier, not part of the
// shared sink API), so — exactly as its own C suite does — this binding re-declares
// them. Keep in step with that file. No struct crosses by value; `img` (the
// versioned-decode code image) is opaque and always null here.
const AttachPidFn = *const fn (c_int, u64, usize, u64, *c_long, ?*anyopaque) callconv(.C) c_int;
const AttachPidTidFn = *const fn (c_int, c_int, u64, usize, u64, *c_long, ?*anyopaque) callconv(.C) c_int;
const AttachJitFn = *const fn (c_int, c_int, u64, usize, ?*anyopaque, u64, u64, *c_long, *c_int, ?*anyopaque) callconv(.C) c_int;

// L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). One operand
// record (include/asmtest_valtrace.h at_val_rec_t), an extern struct matching the
// C ABI exactly (verified via offsetof on this build: kind 0, reg/base/index
// 4/8/12, scale 16, disp 24, addr 32, size 40, is_write/value_valid/wide 42/43/44,
// wide_off 48, value 56, step 64 — 72 bytes).
const AtValRec = extern struct {
    kind: i32 = 0, // at_loc_kind_t
    reg: u32 = 0,
    base: u32 = 0,
    index: u32 = 0,
    scale: i32 = 0,
    disp: i64 = 0,
    addr: u64 = 0,
    size: u16 = 0,
    is_write: bool = false,
    value_valid: bool = false,
    wide: bool = false,
    wide_off: u32 = 0,
    value: u64 = 0,
    step: u32 = 0,
};

// at_loc_kind_t: the location space of an operand.
const LOC_REG: i32 = 0; // a register (key = Capstone reg id)
const LOC_MEM_ABS: i32 = 1; // memory at an absolute effective address (key = addr)
const LOC_MEM_OFF: i32 = 2; // memory at a routine offset

// loc is (kind, key): key is a reg id for LOC_REG, else an absolute address.
fn mkRec(kind: i32, key: u64, is_write: bool) AtValRec {
    var r = AtValRec{};
    r.kind = kind;
    if (kind == LOC_REG) r.reg = @truncate(key) else r.addr = key;
    r.is_write = is_write;
    return r;
}

const ValtraceAppendFn = *const fn (?*anyopaque, u64, ?[*]const AtValRec, usize) callconv(.C) void;
const DefuseBuildFn = *const fn (?*anyopaque) callconv(.C) ?*anyopaque;
const DefuseFreeFn = *const fn (?*anyopaque) callconv(.C) void;
// By-pointer seed variants of asmtest_slice_forward/_backward: only seed.step is
// read, but a pointer argument crosses every FFI uniformly — Zig's extern struct
// CAN pass at_val_rec_t by value, but the by-pointer seed keeps this call's shape
// identical to the six sibling bindings that cannot.
const SliceSeedFn = *const fn (?*anyopaque, *const AtValRec) callconv(.C) ?*anyopaque;
const SliceFreeFn = *const fn (?*anyopaque) callconv(.C) void;
const SliceContainsFn = *const fn (?*anyopaque, u32) callconv(.C) c_int;

// A hand-built (or live-attach-filled) L0 value trace plus its cached L1 def-use
// graph and L2 slicer — mirrors the Python/Ruby/Lua ValueTrace. Holds the function
// pointers the caller already resolved via std.DynLib; owns only `v`/`g`.
const ValueTrace = struct {
    v: ?*anyopaque,
    g: ?*anyopaque = null,
    n_steps: u32 = 0,
    append: ValtraceAppendFn,
    steps_fn: ValtraceStepsFn,
    defuse_build: DefuseBuildFn,
    defuse_free: DefuseFreeFn,
    slice_forward_seed: SliceSeedFn,
    slice_backward_seed: SliceSeedFn,
    slice_free: SliceFreeFn,
    slice_contains: SliceContainsFn,

    // Append one executed instruction at offset `off` with its pre-built operand
    // records (read-set then write-set — see `mkRec`).
    fn step(self: *ValueTrace, off: u64, recs: []const AtValRec) void {
        self.append(self.v, off, if (recs.len > 0) recs.ptr else null, recs.len);
        self.n_steps += 1;
        self.invalidateDefuse();
    }

    // A live producer appends behind our back (unlike `step`, which counts as it
    // goes), so resync the step count and drop any stale def-use graph.
    fn postAttach(self: *ValueTrace) void {
        self.n_steps = @truncate(self.steps_fn(self.v));
        self.invalidateDefuse();
    }

    fn invalidateDefuse(self: *ValueTrace) void {
        if (self.g) |g| {
            self.defuse_free(g);
            self.g = null;
        }
    }

    // The L1 last-writer def-use graph, built once and cached until the next
    // step()/attach invalidates it.
    fn defuse(self: *ValueTrace) !?*anyopaque {
        if (self.g == null) self.g = self.defuse_build(self.v) orelse return error.DefuseBuildFailed;
        return self.g;
    }

    fn sliceSteps(self: *ValueTrace, a: std.mem.Allocator, origin: u32, forward: bool) ![]u32 {
        const g = try self.defuse();
        var seed = AtValRec{};
        seed.step = origin;
        const fn_ = if (forward) self.slice_forward_seed else self.slice_backward_seed;
        const s = fn_(g, &seed) orelse return error.SliceFailed;
        defer self.slice_free(s);
        var out = std.ArrayList(u32).init(a);
        var i: u32 = 0;
        while (i < self.n_steps) : (i += 1) {
            if (self.slice_contains(s, i) != 0) try out.append(i);
        }
        return out.toOwnedSlice();
    }

    // Steps influenced by the value defined at step `origin` (origin included).
    fn forwardSlice(self: *ValueTrace, a: std.mem.Allocator, origin: u32) ![]u32 {
        return self.sliceSteps(a, origin, true);
    }

    // Steps that produced the value used at step `sink` (sink included).
    fn backwardSlice(self: *ValueTrace, a: std.mem.Allocator, sink: u32) ![]u32 {
        return self.sliceSteps(a, sink, false);
    }
};

// The producer's return codes, re-declared for the same reason.
const PTRACE_OK: c_int = 0; // a complete scoped trace
const PTRACE_EINVAL: c_int = -1; // bad arguments
const PTRACE_ENOSYS: c_int = -3; // off Linux x86-64 / no Capstone: tier absent
const PTRACE_ETRACE: c_int = -4; // ptrace/wait failure (seccomp/yama)

var n: u32 = 0;
var failed: bool = false;

fn check(cond: bool, desc: []const u8) void {
    n += 1;
    std.debug.print("{s} {d} - {s}\n", .{ if (cond) "ok" else "not ok", n, desc });
    if (!cond) failed = true;
}

// Both sliceSteps() callers below build their slice in ascending step order
// (sliceSteps walks 0..n_steps), so a plain ordered compare is exact equality.
fn sliceEq(got: []const u32, want: []const u32) bool {
    if (got.len != want.len) return false;
    for (got, want) |g, w| {
        if (g != w) return false;
    }
    return true;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const alloc = gpa.allocator();
    const path = std.process.getEnvVarOwned(alloc, "ASMTEST_DATAFLOW_LIB") catch {
        std.debug.print("# SKIP zig dataflow: ASMTEST_DATAFLOW_LIB unset\n1..0 # skipped\n", .{});
        return;
    };
    defer alloc.free(path);
    var lib = std.DynLib.open(path) catch |err| {
        std.debug.print("# SKIP zig dataflow: cannot dlopen '{s}': {s}\n1..0 # skipped\n", .{ path, @errorName(err) });
        return;
    };
    defer lib.close();

    const gcmove_canon = lib.lookup(GcmoveCanonFn, "asmtest_gcmove_canon") orelse return error.SymNotFound;
    const method_resolve_pc = lib.lookup(MethodResolveFn, "asmtest_method_resolve_pc") orelse return error.SymNotFound;

    // --- GC-move canonicalizer --- //
    check(gcmove_canon(null, 0, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    const mv = [_]GcMove{.{ .old_base = 0x1000, .new_base = 0x2000, .len = 0x100, .step = 5 }};
    check(gcmove_canon(&mv, mv.len, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove_canon(&mv, mv.len, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove_canon(&mv, mv.len, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove_canon(&mv, mv.len, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded");
    check(gcmove_canon(&mv, mv.len, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove_canon(&mv, mv.len, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    const mv2 = [_]GcMove{
        .{ .old_base = 0x1000, .new_base = 0x2000, .len = 0x100, .step = 3 },
        .{ .old_base = 0x2000, .new_base = 0x3000, .len = 0x100, .step = 6 },
    };
    check(gcmove_canon(&mv2, mv2.len, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // --- method resolver --- //
    const ms = [_]Method{
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 3 },
        .{ .addr = 0x2000, .size = 0x20, .name = "Bar", .version = 1 },
        .{ .addr = 0x3000, .size = 0, .name = "Baz", .version = 2 },
    };
    check(method_resolve_pc(&ms, ms.len, 0x1000) == 0, "method: Foo range start");
    check(method_resolve_pc(&ms, ms.len, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method_resolve_pc(&ms, ms.len, 0x1040) == -1, "method: one past Foo -> none");
    check(method_resolve_pc(&ms, ms.len, 0x2010) == 1, "method: Bar range");
    check(method_resolve_pc(&ms, ms.len, 0x3000) == 2, "method: Baz point match");
    check(method_resolve_pc(&ms, ms.len, 0x3001) == -1, "method: Baz is point-only");
    const rj = [_]Method{
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 1 },
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 5 },
    };
    check(method_resolve_pc(&rj, rj.len, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method_resolve_pc(null, 0, 0x1000) == -1, "method: empty map -> -1");

    // ------------------------------------------------------------------
    // T3 — def-use/slice round-trip over a hand-built register-move chain
    // (by-pointer seed, src/dataflow.c). Pure C, no ptrace, so it runs
    // unconditionally — exercises step()/append marshalling and both slicers
    // even where the live-attach section below self-skips.
    // ------------------------------------------------------------------
    {
        const valtrace_new = lib.lookup(ValtraceNewFn, "asmtest_valtrace_new") orelse return error.SymNotFound;
        const valtrace_free = lib.lookup(ValtraceFreeFn, "asmtest_valtrace_free") orelse return error.SymNotFound;
        const valtrace_steps = lib.lookup(ValtraceStepsFn, "asmtest_valtrace_steps") orelse return error.SymNotFound;
        const valtrace_append = lib.lookup(ValtraceAppendFn, "asmtest_valtrace_append") orelse return error.SymNotFound;
        const defuse_build = lib.lookup(DefuseBuildFn, "asmtest_defuse_build") orelse return error.SymNotFound;
        const defuse_free = lib.lookup(DefuseFreeFn, "asmtest_defuse_free") orelse return error.SymNotFound;
        const slice_forward_seed = lib.lookup(SliceSeedFn, "asmtest_slice_forward_seed") orelse return error.SymNotFound;
        const slice_backward_seed = lib.lookup(SliceSeedFn, "asmtest_slice_backward_seed") orelse return error.SymNotFound;
        const slice_free = lib.lookup(SliceFreeFn, "asmtest_slice_free") orelse return error.SymNotFound;
        const slice_contains = lib.lookup(SliceContainsFn, "asmtest_slice_contains") orelse return error.SymNotFound;

        const v = valtrace_new(64, 512, 0);
        defer valtrace_free(v);
        var vt = ValueTrace{
            .v = v,
            .append = valtrace_append,
            .steps_fn = valtrace_steps,
            .defuse_build = defuse_build,
            .defuse_free = defuse_free,
            .slice_forward_seed = slice_forward_seed,
            .slice_backward_seed = slice_backward_seed,
            .slice_free = slice_free,
            .slice_contains = slice_contains,
        };
        // A register chain r10 -> r11 -> r12 (mirrors the Python round-trip test).
        vt.step(0, &[_]AtValRec{mkRec(LOC_REG, 10, true)});
        vt.step(1, &[_]AtValRec{ mkRec(LOC_REG, 10, false), mkRec(LOC_REG, 11, true) });
        vt.step(2, &[_]AtValRec{ mkRec(LOC_REG, 11, false), mkRec(LOC_REG, 12, true) });
        const fwd = try vt.forwardSlice(alloc, 0);
        defer alloc.free(fwd);
        const bwd = try vt.backwardSlice(alloc, 2);
        defer alloc.free(bwd);
        check(sliceEq(fwd, &[_]u32{ 0, 1, 2 }), "slice: forward_slice(0) over r10->r11->r12 == {0,1,2}");
        check(sliceEq(bwd, &[_]u32{ 0, 1, 2 }), "slice: backward_slice(2) over r10->r11->r12 == {0,1,2}");
    }

    // ------------------------------------------------------------------
    // F7 — live-attach data flow over a REAL attached pid.
    //
    // Every assertion is POSITIVE and keyed to something only a working capture can
    // produce (the region's return value, the exact step count, the survival
    // report). Nothing hides behind "if we captured anything" — an EMPTY capture IS
    // the failure signature, so a guard like that skips exactly when it should shout.
    // ------------------------------------------------------------------
    const builtin = @import("builtin");
    // The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a
    // host the live tests MUST run: an unavailable tier there means the lib was
    // linked without Capstone — a build defect that has to be RED, not a skip.
    const live_expected = builtin.os.tag == .linux and builtin.cpu.arch == .x86_64;
    if (!live_expected) {
        std.debug.print("# SKIP live-attach: not linux/x86_64 (the tier is Linux x86-64 only)\n", .{});
    } else {
        const victim = std.process.getEnvVarOwned(alloc, "ASMTEST_DATAFLOW_VICTIM") catch {
            // The lane always exports this; missing means a misconfigured lane, and
            // silently skipping every live test is the hole this suite must not have.
            std.debug.print("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-zig-test`\n", .{});
            std.process.exit(1);
        };
        defer alloc.free(victim);

        const valtrace_new = lib.lookup(ValtraceNewFn, "asmtest_valtrace_new") orelse return error.SymNotFound;
        const valtrace_free = lib.lookup(ValtraceFreeFn, "asmtest_valtrace_free") orelse return error.SymNotFound;
        const valtrace_steps = lib.lookup(ValtraceStepsFn, "asmtest_valtrace_steps") orelse return error.SymNotFound;
        const valtrace_recs = lib.lookup(ValtraceRecsFn, "asmtest_valtrace_recs") orelse return error.SymNotFound;
        const attach_pid = lib.lookup(AttachPidFn, "asmtest_dataflow_ptrace_attach_pid") orelse return error.SymNotFound;
        const attach_pid_tid = lib.lookup(AttachPidTidFn, "asmtest_dataflow_ptrace_attach_pid_tid") orelse return error.SymNotFound;
        const attach_jit = lib.lookup(AttachJitFn, "asmtest_dataflow_ptrace_attach_jit") orelse return error.SymNotFound;
        // T3: the def-use/slice surface over the live capture below — the memory
        // def-use edge the seven bindings could never test before T1's by-pointer seed.
        const valtrace_append = lib.lookup(ValtraceAppendFn, "asmtest_valtrace_append") orelse return error.SymNotFound;
        const defuse_build = lib.lookup(DefuseBuildFn, "asmtest_defuse_build") orelse return error.SymNotFound;
        const defuse_free = lib.lookup(DefuseFreeFn, "asmtest_defuse_free") orelse return error.SymNotFound;
        const slice_forward_seed = lib.lookup(SliceSeedFn, "asmtest_slice_forward_seed") orelse return error.SymNotFound;
        const slice_backward_seed = lib.lookup(SliceSeedFn, "asmtest_slice_backward_seed") orelse return error.SymNotFound;
        const slice_free = lib.lookup(SliceFreeFn, "asmtest_slice_free") orelse return error.SymNotFound;
        const slice_contains = lib.lookup(SliceContainsFn, "asmtest_slice_contains") orelse return error.SymNotFound;

        // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub) — the
        // symbol above resolves either way, so only the return code tells them apart.
        {
            const v = valtrace_new(1, 1, 0);
            var out: c_long = 0;
            const rc = attach_pid(0, 0, 0, 0, &out, v);
            valtrace_free(v);
            check(rc != PTRACE_ENOSYS, "live: tier is real on linux/x86_64 (EINVAL, not ENOSYS)");
        }

        // A live victim: spawn it, learn its region base + its OWN reported pid
        // (see bindings/dataflow_victim.c). `a`/`b` are OURS, so the expected result
        // is a property of THIS run, not a constant a stubbed wrapper could hardcode.
        const Victim = struct {
            child: std.process.Child,
            base: u64,
            len: usize,
            pid: c_int,
            counter_path: []const u8,

            fn spawn(a: std.mem.Allocator, exe: []const u8, tag: []const u8, x: i64, y: i64) !@This() {
                const cpath = try std.fmt.allocPrint(a, "/tmp/asmtest-df-zig-{s}.counter", .{tag});
                const xs = try std.fmt.allocPrint(a, "{d}", .{x});
                defer a.free(xs);
                const ys = try std.fmt.allocPrint(a, "{d}", .{y});
                defer a.free(ys);
                var child = std.process.Child.init(&[_][]const u8{ exe, cpath, xs, ys }, a);
                child.stdout_behavior = .Pipe;
                try child.spawn();
                // Blocks until the victim flushes its handshake and starts looping.
                const line = try child.stdout.?.reader().readUntilDelimiterAlloc(a, '\n', 256);
                defer a.free(line);
                var it = std.mem.tokenizeScalar(u8, line, ' ');
                const base_tok = it.next() orelse return error.BadHandshake;
                const len_tok = it.next() orelse return error.BadHandshake;
                const pid_tok = it.next() orelse return error.BadHandshake;
                return .{
                    .child = child,
                    .base = try std.fmt.parseInt(u64, base_tok["base=0x".len..], 16),
                    .len = try std.fmt.parseInt(usize, len_tok["len=".len..], 10),
                    .pid = try std.fmt.parseInt(c_int, pid_tok["pid=".len..], 10),
                    .counter_path = cpath,
                };
            }
            fn counter(self: *@This()) u64 {
                const f = std.fs.cwd().openFile(self.counter_path, .{}) catch return 0;
                defer f.close();
                var buf: [8]u8 = undefined;
                const got = f.readAll(&buf) catch return 0;
                if (got != 8) return 0;
                return std.mem.readInt(u64, &buf, .little);
            }
            fn close(self: *@This(), a: std.mem.Allocator) void {
                _ = self.child.kill() catch {};
                a.free(self.counter_path);
            }
        };

        // ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
        // (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
        // PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
        const H = struct {
            fn checkRc(rc: c_int, desc: []const u8) void {
                if (rc == PTRACE_ETRACE)
                    std.debug.print("# {s}: ptrace refused (ETRACE) — the lane needs " ++
                        "--cap-add=SYS_PTRACE; this is NOT a valid skip\n", .{desc});
                check(rc == PTRACE_OK, desc);
            }
        };

        {
            var vic = try Victim.spawn(alloc, victim, "1", 7, 5);
            const v = valtrace_new(64, 512, 0);
            var out: c_long = 0;
            H.checkRc(attach_pid(vic.pid, vic.base, vic.len, 0, &out, v),
                "live: attach_pid a FOREIGN running pid + stepped the region");
            // The region really executed IN the victim: rax = rdi + rsi.
            check(out == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)");
            // Exactly df_chain's six in-region instructions — not "some".
            check(valtrace_steps(v) == 6, "live: six in-region steps captured over the victim");
            check(valtrace_recs(v) > 0, "live: operand records captured");
            // SURVIVAL: we attached to a process we do not own; it must outlive the detach.
            const c0 = vic.counter();
            std.time.sleep(50 * std.time.ns_per_ms);
            check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)");
            // T3: the memory def-use edge (step1 store -> step2 load) — only
            // reachable via the by-pointer slice seed (T1); the seven bindings
            // could never test this before.
            var lvt = ValueTrace{
                .v = v,
                .append = valtrace_append,
                .steps_fn = valtrace_steps,
                .defuse_build = defuse_build,
                .defuse_free = defuse_free,
                .slice_forward_seed = slice_forward_seed,
                .slice_backward_seed = slice_backward_seed,
                .slice_free = slice_free,
                .slice_contains = slice_contains,
            };
            lvt.postAttach();
            const lfwd = try lvt.forwardSlice(alloc, 0);
            defer alloc.free(lfwd);
            const lbwd = try lvt.backwardSlice(alloc, 4);
            defer alloc.free(lbwd);
            check(sliceEq(lfwd, &[_]u32{ 0, 1, 2, 3, 4 }),
                "live: forward_slice(0) == {0,1,2,3,4} over df_chain, excludes ret");
            check(sliceEq(lbwd, &[_]u32{ 0, 1, 2, 3, 4 }),
                "live: backward_slice(4) == {0,1,2,3,4} -- the memory edge step1(store)->step2(load), excludes ret");
            valtrace_free(v);
            vic.close(alloc);
        }
        {
            // THE anti-hardcode control: a second victim, different args, same wrapper.
            var vic = try Victim.spawn(alloc, victim, "2", 17, 25);
            const v = valtrace_new(64, 512, 0);
            var out: c_long = 0;
            H.checkRc(attach_pid(vic.pid, vic.base, vic.len, 0, &out, v),
                "live: attach_pid the second victim");
            check(out == 42, "live: result TRACKS the victim's args (17+25=42)");
            check(valtrace_steps(v) == 6, "live: six steps on the second victim too");
            valtrace_free(v);
            vic.close(alloc);
        }
        {
            var vic = try Victim.spawn(alloc, victim, "3", 9, 4);
            const v = valtrace_new(64, 512, 0);
            var out: c_long = 0;
            // only_tid 0: step whichever thread enters the region (here, the only one).
            H.checkRc(attach_pid_tid(vic.pid, 0, vic.base, vic.len, 0, &out, v),
                "live: attach_pid_tid stepped the entering thread");
            check(out == 13, "live: attach_pid_tid region returned 13 (9+4)");
            check(valtrace_steps(v) == 6, "live: attach_pid_tid captured six steps");
            valtrace_free(v);
            vic.close(alloc);
        }
        {
            var vic = try Victim.spawn(alloc, victim, "4", 20, 3);
            const v = valtrace_new(64, 512, 0);
            var out: c_long = 0;
            var survived: c_int = 0;
            H.checkRc(attach_jit(vic.pid, 0, vic.base, vic.len, null, 0, 0, &out, &survived, v),
                "live: attach_jit stepped the region");
            check(out == 23, "live: attach_jit region returned 23 (20+3)");
            check(valtrace_steps(v) == 6, "live: attach_jit captured six steps");
            // The producer's OWN survival report — the house rule that a foreign
            // target is never killed, asserted from its side.
            check(survived == 1, "live: attach_jit reported the target as survived");
            const c0 = vic.counter();
            std.time.sleep(50 * std.time.ns_per_ms);
            check(vic.counter() > c0, "live: attach_jit victim kept running after detach");
            valtrace_free(v);
            vic.close(alloc);
        }
        {
            // Negative control: the wrapper must surface the producer's rejections
            // rather than manufacture success.
            const v = valtrace_new(8, 8, 0);
            var out: c_long = 0;
            check(attach_pid(12345, 0x1000, 0, 0, &out, v) == PTRACE_EINVAL,
                "live: zero-length region is rejected (EINVAL)");
            check(attach_pid(0, 0x1000, 21, 0, &out, v) == PTRACE_EINVAL,
                "live: pid 0 is rejected (EINVAL)");
            check(attach_pid(0x7FFFFFF0, 0x1000, 21, 0, &out, v) != PTRACE_OK,
                "live: attaching to a nonexistent pid never returns OK");
            valtrace_free(v);
        }
    }

    std.debug.print("1..{d}\n", .{n});
    if (failed) std.process.exit(1);
}
