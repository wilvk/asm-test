# frozen_string_literal: true

# Ruby data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
# mirroring the Python/C++/Node suites' semantics.
require_relative "dataflow"

DF = Asmtest::DataFlow
$n = 0
$failed = false

def check(cond, desc)
  $n += 1
  puts((cond ? "ok " : "not ok ") + $n.to_s + " - " + desc)
  $failed = true unless cond
end

# --- GC-move canonicalizer (forward-to-final) --- #
check(DF.gcmove_canon([], 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
mv = [[0x1000, 0x2000, 0x100, 5]]
check(DF.gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
check(DF.gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
check(DF.gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
check(DF.gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
check(DF.gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
check(DF.gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
mv2 = [[0x1000, 0x2000, 0x100, 3], [0x2000, 0x3000, 0x100, 6]]
check(DF.gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

# --- method resolver (tiered re-JIT aware) --- #
ms = [[0x1000, 0x40, "Foo", 3], [0x2000, 0x20, "Bar", 1], [0x3000, 0, "Baz", 2]]
check(DF.method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start")
check(DF.method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
check(DF.method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none")
check(DF.method_resolve_pc(ms, 0x2010) == 1, "method: Bar range")
check(DF.method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match")
check(DF.method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only")
rj = [[0x1000, 0x40, "Foo", 1], [0x1000, 0x40, "Foo", 5]]
check(DF.method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
check(DF.method_resolve_pc([], 0x1000) == -1, "method: empty map -> -1")

# ---------------------------------------------------------------------------
# T3 — def-use/slice round-trip over a hand-built register-move chain (the
# by-pointer seed from T1, wrapped in T2). Pure `step`/`append` marshalling, no
# ptrace, so it runs even where the live-attach section below self-skips.
# ---------------------------------------------------------------------------
vt = DF::ValueTrace.new(8, 8)
vt.step(0x00, reads: [], writes: [[DF::LOC_REG, 10]])                    # def r10
vt.step(0x03, reads: [[DF::LOC_REG, 10]], writes: [[DF::LOC_REG, 11]])   # r11 <- r10
vt.step(0x06, reads: [[DF::LOC_REG, 11]], writes: [[DF::LOC_REG, 12]])   # r12 <- r11
check(vt.forward_slice(0) == Set[0, 1, 2],
      "slice: forward_slice(0) over r10->r11->r12 == {0,1,2}")
check(vt.backward_slice(2) == Set[0, 1, 2],
      "slice: backward_slice(2) over r10->r11->r12 == {0,1,2}")
vt.free

# ---------------------------------------------------------------------------
# T4 — the code-image recorder (asmtest_codeimage.h): track a buffer in THIS
# process and read back the exact bytes it snapshotted. Runs wherever
# soft-dirty page tracking is available -- no ptrace, so it runs even where
# the live-attach section below self-skips.
# ---------------------------------------------------------------------------
if !DF.codeimage_available?
  puts "# SKIP codeimage: #{DF.codeimage_skip_reason}"
else
  cbuf = Fiddle::Pointer.malloc(16)
  pattern = (0...16).map { |i| 0xA0 + i }.pack("C*")
  cbuf[0, 16] = pattern
  img = DF::CodeImage.new(0)
  trc = img.track(cbuf.to_i, 16)
  check(trc == DF::CI_OK, "codeimage: track() snapshots v0")
  t0 = img.now
  check(t0 > 0, "codeimage: now() advanced past 0 after track")
  got = img.bytes_at(cbuf.to_i, t0)
  check(got == pattern, "codeimage: bytes_at() returns the exact tracked bytes")
  img.free
end

# ---------------------------------------------------------------------------
# F7 — live-attach data flow: capture over a REAL attached pid.
#
# Every assertion is POSITIVE and keyed to something only a working capture can
# produce (the region's return value, the exact step count, the survival report).
# Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
# signature, so a guard like that would skip exactly when it should shout.
# ---------------------------------------------------------------------------

# The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a host
# the live tests MUST run: an unavailable tier there means the lib was linked
# without Capstone — a build defect that has to be RED, not a skip.
LIVE_EXPECTED = RUBY_PLATFORM.include?("linux") && RUBY_PLATFORM.include?("x86_64")
VICTIM = ENV["ASMTEST_DATAFLOW_VICTIM"]

# A live victim: spawn it, learn its region base + pid. `a`/`b` are OURS, so the
# expected result is a property of THIS run — a wrapper that hardcodes an answer
# cannot satisfy two victims with different args.
class Victim
  attr_reader :pid, :base, :len

  def initialize(tag, a, b)
    @counter_path = "/tmp/asmtest-df-ruby-#{tag}.counter"
    @io = IO.popen([VICTIM, @counter_path, a.to_s, b.to_s])
    line = @io.gets.to_s.strip # blocks until the victim is looping
    m = /\Abase=0x([0-9a-f]+) len=(\d+) pid=(\d+)\z/.match(line)
    raise "victim handshake failed: #{line.inspect}" unless m

    @base = m[1].to_i(16)
    @len = m[2].to_i
    @pid = m[3].to_i # the victim's OWN pid (see bindings/dataflow_victim.c)
  end

  def counter
    File.binread(@counter_path, 8).unpack1("Q<")
  end

  def close
    Process.kill("KILL", @pid)
  rescue Errno::ESRCH
    nil
  ensure
    @io.close rescue nil
  end
end

# ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
# (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
# PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
def check_rc(rc, what)
  if rc == DF::PTRACE_ETRACE
    puts "# #{what}: ptrace refused (ETRACE) — the lane needs --cap-add=SYS_PTRACE; " \
         "this is NOT a valid skip"
  end
  check(rc == DF::PTRACE_OK, what)
end

if !LIVE_EXPECTED
  puts "# SKIP live-attach: not linux/x86_64 (the tier is Linux x86-64 only)"
elsif VICTIM.nil?
  # The lane always exports this; missing means a misconfigured lane, and silently
  # skipping every live test is the hole this suite must not have.
  puts "Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-ruby-test`"
  exit(1)
else
  # Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub).
  check(DF.live_attach_available?, "live: tier is real on linux/x86_64 (EINVAL, not ENOSYS)")

  vic = Victim.new("1", 7, 5)
  vt = DF::ValueTrace.new(64, 512)
  rc, result = vt.attach_pid(vic.pid, vic.base, vic.len)
  check_rc(rc, "live: attach_pid a FOREIGN running pid + stepped the region")
  # The region really executed IN the victim: rax = rdi + rsi.
  check(result == 12, "live: attach_pid region returned 12 (got #{result})")
  # Exactly df_chain's six in-region instructions — not "some".
  check(vt.steps == 6, "live: six in-region steps captured (got #{vt.steps})")
  check(vt.recs > 0, "live: operand records captured")
  # SURVIVAL: we attached to a process we do not own; it must outlive the detach.
  c0 = vic.counter
  sleep 0.05
  check(vic.counter > c0, "live: victim SURVIVED the detach (counter advanced)")
  # T3 — the memory def-use edge (step1 store -> step2 load) reached from step4
  # through the load at step2: the by-pointer seed (T1) is what makes this
  # slice reachable at all in this binding.
  fwd0 = vt.forward_slice(0)
  bwd4 = vt.backward_slice(4)
  check(fwd0 == Set[0, 1, 2, 3, 4],
        "live: forward_slice(0) == {0,1,2,3,4} over df_chain, excludes ret (got #{fwd0.to_a.sort})")
  check(bwd4 == Set[0, 1, 2, 3, 4],
        "live: backward_slice(4) == {0,1,2,3,4} -- the memory edge step1(store)->step2(load), " \
        "excludes ret (got #{bwd4.to_a.sort})")
  vt.free
  vic.close

  # THE anti-hardcode control: a second victim, different args, same wrapper.
  vic = Victim.new("2", 17, 25)
  vt = DF::ValueTrace.new(64, 512)
  rc, result = vt.attach_pid(vic.pid, vic.base, vic.len)
  check_rc(rc, "live: attach_pid the second victim")
  check(result == 42, "live: result TRACKS the victim's args (17+25=42, got #{result})")
  check(vt.steps == 6, "live: six steps on the second victim too")
  vt.free
  vic.close

  vic = Victim.new("3", 9, 4)
  vt = DF::ValueTrace.new(64, 512)
  # only_tid 0: step whichever thread enters the region (here, the only one).
  rc, result = vt.attach_pid_tid(vic.pid, 0, vic.base, vic.len)
  check_rc(rc, "live: attach_pid_tid stepped the entering thread")
  check(result == 13, "live: attach_pid_tid region returned 13 (got #{result})")
  check(vt.steps == 6, "live: attach_pid_tid captured six steps")
  vt.free
  vic.close

  vic = Victim.new("4", 20, 3)
  vt = DF::ValueTrace.new(64, 512)
  rc, result, survived = vt.attach_jit(vic.pid, 0, vic.base, vic.len)
  check_rc(rc, "live: attach_jit stepped the region")
  check(result == 23, "live: attach_jit region returned 23 (got #{result})")
  check(vt.steps == 6, "live: attach_jit captured six steps")
  # The producer's OWN survival report — the house rule that a foreign target is
  # never killed, asserted from its side.
  check(survived == 1, "live: attach_jit reported the target as survived")
  c0 = vic.counter
  sleep 0.05
  check(vic.counter > c0, "live: attach_jit victim kept running after detach")
  vt.free
  vic.close

  # T4 — a real code-image threaded through attach_pid_versioned: build the
  # recorder over the victim's OWN published region, then decode the capture
  # against it. A non-NULL img must not break the capture or land in the
  # wrong argument slot (a dropped/misplaced pointer would corrupt base/pid
  # and the result assert below would catch it).
  if DF.codeimage_available?
    vic = Victim.new("5", 11, 6)
    img = DF::CodeImage.new(vic.pid)
    trc = img.track(vic.base, vic.len)
    check(trc == DF::CI_OK, "codeimage: track() over the victim's published region")
    vt = DF::ValueTrace.new(64, 512)
    rc, result = vt.attach_pid_versioned(vic.pid, vic.base, vic.len, img, img.now)
    check_rc(rc, "live: attach_pid_versioned with a real img")
    check(result == 17,
          "live: attach_pid_versioned result TRACKS the victim's args (11+6=17, got #{result})")
    check(vt.steps == 6, "live: attach_pid_versioned captured six steps with a real img")
    img.free
    vt.free
    vic.close
  else
    puts "# SKIP codeimage live: #{DF.codeimage_skip_reason}"
  end

  # Negative control: the wrapper must surface the producer's rejections rather
  # than manufacture success.
  vt = DF::ValueTrace.new(8, 8)
  check(vt.attach_pid(12345, 0x1000, 0)[0] == DF::PTRACE_EINVAL,
        "live: zero-length region is rejected (EINVAL)")
  check(vt.attach_pid(0, 0x1000, 21)[0] == DF::PTRACE_EINVAL,
        "live: pid 0 is rejected (EINVAL)")
  check(vt.attach_pid(0x7FFFFFF0, 0x1000, 21)[0] != DF::PTRACE_OK,
        "live: attaching to a nonexistent pid never returns OK")
  vt.free
end

puts "1..#{$n}"
exit($failed ? 1 : 0)
