// Rust data-flow binding smoke (Phase 6 + F7): GC-move canonicalizer + method
// resolver, mirroring the other language suites — and (F7) a REAL live attach to a
// victim process by pid. Direct FFI (extern "C" + #[repr(C)]); built standalone
// with `rustc test_dataflow.rs -L <build> -l asmtest_dataflow`.
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

#[repr(C)]
struct GcMove {
    old_base: u64,
    new_base: u64,
    len: u64,
    step: u32,
}

#[repr(C)]
struct Method {
    addr: u64,
    size: u64,
    name: *const c_char,
    version: u64,
}

extern "C" {
    fn asmtest_gcmove_canon(moves: *const GcMove, nmoves: usize, step: u32, phys: u64) -> u64;
    fn asmtest_method_resolve_pc(methods: *const Method, nmethods: usize, pc: u64) -> c_int;

    // The L0 sink handle is opaque — passed around, never inspected.
    fn asmtest_valtrace_new(steps_cap: usize, recs_cap: usize, wide_cap: usize) -> *mut c_void;
    fn asmtest_valtrace_free(v: *mut c_void);
    fn asmtest_valtrace_steps(v: *const c_void) -> usize;
    fn asmtest_valtrace_recs(v: *const c_void) -> usize;

    // F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c). The
    // producer ships NO header on purpose (a value-trace PRODUCER is a tier, not
    // part of the shared sink API), so — exactly as its own C suite does — this
    // binding re-declares them. Keep in step with that file. No struct crosses by
    // value; `img` (the versioned-decode code image) is opaque and always null here.
    fn asmtest_dataflow_ptrace_attach_pid(
        pid: c_int, base: u64, code_len: usize, max_insns: u64, result: *mut c_long,
        vt: *mut c_void,
    ) -> c_int;
    fn asmtest_dataflow_ptrace_attach_pid_tid(
        pid: c_int, only_tid: c_int, base: u64, code_len: usize, max_insns: u64,
        result: *mut c_long, vt: *mut c_void,
    ) -> c_int;
    fn asmtest_dataflow_ptrace_attach_jit(
        pid: c_int, only_tid: c_int, base: u64, code_len: usize, img: *mut c_void,
        when: u64, max_insns: u64, result: *mut c_long, survived: *mut c_int,
        vt: *mut c_void,
    ) -> c_int;
}

// The producer's return codes, re-declared for the same reason.
const PTRACE_OK: c_int = 0; // a complete scoped trace
const PTRACE_EINVAL: c_int = -1; // bad arguments
const PTRACE_ENOSYS: c_int = -3; // off Linux x86-64 / no Capstone: tier absent
const PTRACE_ETRACE: c_int = -4; // ptrace/wait failure (seccomp/yama)

static N: AtomicU32 = AtomicU32::new(0);
static FAILED: AtomicBool = AtomicBool::new(false);

fn check(cond: bool, desc: &str) {
    let n = N.fetch_add(1, Ordering::SeqCst) + 1;
    println!("{} {} - {}", if cond { "ok" } else { "not ok" }, n, desc);
    if !cond {
        FAILED.store(true, Ordering::SeqCst);
    }
}

fn gcmove(moves: &[(u64, u64, u64, u32)], step: u32, phys: u64) -> u64 {
    let arr: Vec<GcMove> = moves
        .iter()
        .map(|&(o, n, l, s)| GcMove { old_base: o, new_base: n, len: l, step: s })
        .collect();
    let ptr = if arr.is_empty() { std::ptr::null() } else { arr.as_ptr() };
    unsafe { asmtest_gcmove_canon(ptr, arr.len(), step, phys) }
}

fn method(methods: &[(u64, u64, &str, u64)], pc: u64) -> c_int {
    let names: Vec<CString> = methods.iter().map(|&(_, _, n, _)| CString::new(n).unwrap()).collect();
    let arr: Vec<Method> = methods
        .iter()
        .enumerate()
        .map(|(i, &(a, sz, _, v))| Method { addr: a, size: sz, name: names[i].as_ptr(), version: v })
        .collect();
    let ptr = if arr.is_empty() { std::ptr::null() } else { arr.as_ptr() };
    let r = unsafe { asmtest_method_resolve_pc(ptr, arr.len(), pc) };
    drop(names); // keep the CStrings alive through the call
    r
}

fn main() {
    // GC-move canonicalizer
    check(gcmove(&[], 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    let mv = [(0x1000, 0x2000, 0x100, 5)];
    check(gcmove(&mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove(&mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove(&mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove(&mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded");
    check(gcmove(&mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove(&mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    let mv2 = [(0x1000, 0x2000, 0x100, 3), (0x2000, 0x3000, 0x100, 6)];
    check(gcmove(&mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // method resolver
    let ms = [(0x1000, 0x40, "Foo", 3), (0x2000, 0x20, "Bar", 1), (0x3000, 0, "Baz", 2)];
    check(method(&ms, 0x1000) == 0, "method: Foo range start");
    check(method(&ms, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method(&ms, 0x1040) == -1, "method: one past Foo -> none");
    check(method(&ms, 0x2010) == 1, "method: Bar range");
    check(method(&ms, 0x3000) == 2, "method: Baz point match");
    check(method(&ms, 0x3001) == -1, "method: Baz is point-only");
    let rj = [(0x1000, 0x40, "Foo", 1), (0x1000, 0x40, "Foo", 5)];
    check(method(&rj, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method(&[], 0x1000) == -1, "method: empty map -> -1");

    live_attach_tests();

    println!("1..{}", N.load(Ordering::SeqCst));
    if FAILED.load(Ordering::SeqCst) {
        std::process::exit(1);
    }
}

// ---------------------------------------------------------------------------
// F7 — live-attach data flow: capture over a REAL attached pid.
//
// Every assertion is POSITIVE and keyed to something only a working capture can
// produce (the region's return value, the exact step count, the survival report).
// Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
// signature, so a guard like that would skip exactly when it should shout.
// ---------------------------------------------------------------------------

// A live victim: spawn it, learn its region base + its OWN reported pid (see
// bindings/dataflow_victim.c). `a`/`b` are OURS, so the expected result is a
// property of THIS run, not a constant a stubbed wrapper could hardcode.
struct Victim {
    child: std::process::Child,
    base: u64,
    len: usize,
    pid: c_int,
    counter_path: String,
}

impl Victim {
    fn spawn(exe: &str, tag: &str, a: i64, b: i64) -> Victim {
        use std::io::BufRead;
        let counter_path = format!("/tmp/asmtest-df-rust-{}.counter", tag);
        let mut child = std::process::Command::new(exe)
            .arg(&counter_path)
            .arg(a.to_string())
            .arg(b.to_string())
            .stdout(std::process::Stdio::piped())
            .spawn()
            .expect("spawn victim");
        // Blocks until the victim flushes its handshake and starts looping.
        let mut line = String::new();
        std::io::BufReader::new(child.stdout.as_mut().unwrap())
            .read_line(&mut line)
            .expect("victim handshake");
        let mut it = line.trim().split_whitespace();
        let base = it.next().and_then(|t| t.strip_prefix("base=0x"))
            .and_then(|h| u64::from_str_radix(h, 16).ok())
            .unwrap_or_else(|| panic!("victim handshake failed: {:?}", line));
        let len = it.next().and_then(|t| t.strip_prefix("len="))
            .and_then(|d| d.parse::<usize>().ok())
            .unwrap_or_else(|| panic!("victim handshake failed: {:?}", line));
        let pid = it.next().and_then(|t| t.strip_prefix("pid="))
            .and_then(|d| d.parse::<c_int>().ok())
            .unwrap_or_else(|| panic!("victim handshake failed: {:?}", line));
        Victim { child, base, len, pid, counter_path }
    }
    fn counter(&self) -> u64 {
        let b = std::fs::read(&self.counter_path).unwrap_or_default();
        if b.len() < 8 { return 0; }
        // Built without --edition, i.e. edition 2015, whose prelude has no TryInto.
        let mut le = [0u8; 8];
        le.copy_from_slice(&b[..8]);
        u64::from_le_bytes(le)
    }
    fn close(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

// ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
// (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
// PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
fn check_rc(rc: c_int, desc: &str) {
    if rc == PTRACE_ETRACE {
        println!("# {}: ptrace refused (ETRACE) — the lane needs --cap-add=SYS_PTRACE; \
                  this is NOT a valid skip", desc);
    }
    check(rc == PTRACE_OK, desc);
}

fn live_attach_tests() {
    // The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a
    // host the live tests MUST run: an unavailable tier there means the lib was
    // linked without Capstone — a build defect that has to be RED, not a skip.
    if !(cfg!(target_os = "linux") && cfg!(target_arch = "x86_64")) {
        println!("# SKIP live-attach: not linux/x86_64 (the tier is Linux x86-64 only)");
        return;
    }
    let exe = match std::env::var("ASMTEST_DATAFLOW_VICTIM") {
        Ok(v) => v,
        Err(_) => {
            // The lane always exports this; missing means a misconfigured lane, and
            // silently skipping every live test is the hole this suite must not have.
            println!("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-rust-test`");
            std::process::exit(1);
        }
    };

    unsafe {
        // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub) — the
        // symbol links either way, so only the return code tells them apart.
        let v = asmtest_valtrace_new(1, 1, 0);
        let mut out: c_long = 0;
        let rc = asmtest_dataflow_ptrace_attach_pid(0, 0, 0, 0, &mut out, v);
        asmtest_valtrace_free(v);
        check(rc != PTRACE_ENOSYS, "live: tier is real on linux/x86_64 (EINVAL, not ENOSYS)");
    }

    unsafe {
        let mut vic = Victim::spawn(&exe, "1", 7, 5);
        let v = asmtest_valtrace_new(64, 512, 0);
        let mut out: c_long = 0;
        check_rc(
            asmtest_dataflow_ptrace_attach_pid(vic.pid, vic.base, vic.len, 0, &mut out, v),
            "live: attach_pid a FOREIGN running pid + stepped the region",
        );
        // The region really executed IN the victim: rax = rdi + rsi.
        check(out == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)");
        // Exactly df_chain's six in-region instructions — not "some".
        check(asmtest_valtrace_steps(v) == 6, "live: six in-region steps captured over the victim");
        check(asmtest_valtrace_recs(v) > 0, "live: operand records captured");
        // SURVIVAL: we attached to a process we do not own; it must outlive the detach.
        let c0 = vic.counter();
        std::thread::sleep(std::time::Duration::from_millis(50));
        check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)");
        asmtest_valtrace_free(v);
        vic.close();
    }
    unsafe {
        // THE anti-hardcode control: a second victim, different args, same wrapper.
        let mut vic = Victim::spawn(&exe, "2", 17, 25);
        let v = asmtest_valtrace_new(64, 512, 0);
        let mut out: c_long = 0;
        check_rc(
            asmtest_dataflow_ptrace_attach_pid(vic.pid, vic.base, vic.len, 0, &mut out, v),
            "live: attach_pid the second victim",
        );
        check(out == 42, "live: result TRACKS the victim's args (17+25=42)");
        check(asmtest_valtrace_steps(v) == 6, "live: six steps on the second victim too");
        asmtest_valtrace_free(v);
        vic.close();
    }
    unsafe {
        let mut vic = Victim::spawn(&exe, "3", 9, 4);
        let v = asmtest_valtrace_new(64, 512, 0);
        let mut out: c_long = 0;
        // only_tid 0: step whichever thread enters the region (here, the only one).
        check_rc(
            asmtest_dataflow_ptrace_attach_pid_tid(vic.pid, 0, vic.base, vic.len, 0, &mut out, v),
            "live: attach_pid_tid stepped the entering thread",
        );
        check(out == 13, "live: attach_pid_tid region returned 13 (9+4)");
        check(asmtest_valtrace_steps(v) == 6, "live: attach_pid_tid captured six steps");
        asmtest_valtrace_free(v);
        vic.close();
    }
    unsafe {
        let mut vic = Victim::spawn(&exe, "4", 20, 3);
        let v = asmtest_valtrace_new(64, 512, 0);
        let mut out: c_long = 0;
        let mut survived: c_int = 0;
        check_rc(
            asmtest_dataflow_ptrace_attach_jit(
                vic.pid, 0, vic.base, vic.len, std::ptr::null_mut(), 0, 0,
                &mut out, &mut survived, v,
            ),
            "live: attach_jit stepped the region",
        );
        check(out == 23, "live: attach_jit region returned 23 (20+3)");
        check(asmtest_valtrace_steps(v) == 6, "live: attach_jit captured six steps");
        // The producer's OWN survival report — the house rule that a foreign target
        // is never killed, asserted from its side.
        check(survived == 1, "live: attach_jit reported the target as survived");
        let c0 = vic.counter();
        std::thread::sleep(std::time::Duration::from_millis(50));
        check(vic.counter() > c0, "live: attach_jit victim kept running after detach");
        asmtest_valtrace_free(v);
        vic.close();
    }
    unsafe {
        // Negative control: the wrapper must surface the producer's rejections
        // rather than manufacture success.
        let v = asmtest_valtrace_new(8, 8, 0);
        let mut out: c_long = 0;
        check(asmtest_dataflow_ptrace_attach_pid(12345, 0x1000, 0, 0, &mut out, v) == PTRACE_EINVAL,
            "live: zero-length region is rejected (EINVAL)");
        check(asmtest_dataflow_ptrace_attach_pid(0, 0x1000, 21, 0, &mut out, v) == PTRACE_EINVAL,
            "live: pid 0 is rejected (EINVAL)");
        check(asmtest_dataflow_ptrace_attach_pid(0x7FFFFFF0, 0x1000, 21, 0, &mut out, v) != PTRACE_OK,
            "live: attaching to a nonexistent pid never returns OK");
        asmtest_valtrace_free(v);
    }
}
