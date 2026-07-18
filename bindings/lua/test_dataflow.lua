-- Lua data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
-- mirroring the Python/C++/Node/Ruby suites. Run under LuaJIT.
local df = require("dataflow")

local n = 0
local failed = false
local function check(cond, desc)
  n = n + 1
  print((cond and "ok " or "not ok ") .. n .. " - " .. desc)
  if not cond then failed = true end
end

-- GC-move canonicalizer
check(df.gcmove_canon({}, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
local mv = { { 0x1000, 0x2000, 0x100, 5 } }
check(df.gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
check(df.gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
check(df.gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
check(df.gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
check(df.gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
check(df.gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
local mv2 = { { 0x1000, 0x2000, 0x100, 3 }, { 0x2000, 0x3000, 0x100, 6 } }
check(df.gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

-- method resolver
local ms = { { 0x1000, 0x40, "Foo", 3 }, { 0x2000, 0x20, "Bar", 1 }, { 0x3000, 0, "Baz", 2 } }
check(df.method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start")
check(df.method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
check(df.method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none")
check(df.method_resolve_pc(ms, 0x2010) == 1, "method: Bar range")
check(df.method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match")
check(df.method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only")
local rj = { { 0x1000, 0x40, "Foo", 1 }, { 0x1000, 0x40, "Foo", 5 } }
check(df.method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
check(df.method_resolve_pc({}, 0x1000) == -1, "method: empty map -> -1")

-- ---------------------------------------------------------------------------
-- T3 — def-use/slice round-trip over a hand-built register-move chain (the
-- by-pointer seed from T1, wrapped in T2). Pure step()/append marshalling, no
-- ptrace, so it runs even where the live-attach section below self-skips.
-- ---------------------------------------------------------------------------
local function list_eq(got, want)
  if #got ~= #want then return false end
  for i, v in ipairs(want) do
    if got[i] ~= v then return false end
  end
  return true
end

do
  local vt = df.ValueTrace(8, 8)
  vt:step(0x00, {}, { { df.LOC_REG, 10 } })                        -- def r10
  vt:step(0x03, { { df.LOC_REG, 10 } }, { { df.LOC_REG, 11 } })     -- r11 <- r10
  vt:step(0x06, { { df.LOC_REG, 11 } }, { { df.LOC_REG, 12 } })     -- r12 <- r11
  check(list_eq(vt:forward_slice(0), { 0, 1, 2 }),
        "slice: forward_slice(0) over r10->r11->r12 == {0,1,2}")
  check(list_eq(vt:backward_slice(2), { 0, 1, 2 }),
        "slice: backward_slice(2) over r10->r11->r12 == {0,1,2}")
  vt:free()
end

-- ---------------------------------------------------------------------------
-- T4 — the code-image recorder (asmtest_codeimage.h): track a buffer in THIS
-- process and read back the exact bytes it snapshotted. Runs wherever
-- soft-dirty page tracking is available -- no ptrace, so it runs even where
-- the live-attach section below self-skips.
-- ---------------------------------------------------------------------------
if not df.codeimage_available() then
  print("# SKIP codeimage: " .. df.codeimage_skip_reason())
else
  local ffi2 = require("ffi")
  local cbuf = ffi2.new("uint8_t[16]")
  for i = 0, 15 do cbuf[i] = 0xA0 + i end
  local img = df.CodeImage(0)
  local trc = img:track(cbuf, 16)
  check(trc == df.CI_OK, "codeimage: track() snapshots v0")
  local t0 = img:now()
  check(t0 > 0, "codeimage: now() advanced past 0 after track")
  local got = img:bytes_at(cbuf, t0)
  check(got == ffi2.string(cbuf, 16), "codeimage: bytes_at() returns the exact tracked bytes")
  img:free()
end

-- ---------------------------------------------------------------------------
-- F7 — live-attach data flow: capture over a REAL attached pid.
--
-- Every assertion is POSITIVE and keyed to something only a working capture can
-- produce (the region's return value, the exact step count, the survival report).
-- Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
-- signature, so a guard like that would skip exactly when it should shout.
-- ---------------------------------------------------------------------------

-- The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a host
-- the live tests MUST run: an unavailable tier there means the lib was linked
-- without Capstone — a build defect that has to be RED, not a skip.
local ffi_os = require("ffi").os
local ffi_arch = require("ffi").arch
local live_expected = ffi_os == "Linux" and ffi_arch == "x64"
local victim_exe = os.getenv("ASMTEST_DATAFLOW_VICTIM")

-- A live victim: spawn it, learn its region base + pid. io.popen runs the command
-- under `sh -c` and hands back only a stream, so the victim's OWN reported pid (see
-- bindings/dataflow_victim.c) is the only correct one here — `exec` keeps the pid
-- stable anyway, but we use the reported one regardless. `a`/`b` are OURS, so the
-- expected result is a property of THIS run, not a constant a stub could hardcode.
local function spawn_victim(tag, a, b)
  local counter_path = "/tmp/asmtest-df-lua-" .. tag .. ".counter"
  local cmd = string.format("exec %s %s %d %d", victim_exe, counter_path, a, b)
  local pipe = assert(io.popen(cmd, "r"))
  local line = pipe:read("*l") -- blocks until the victim is looping
  local base, len, pid = string.match(line or "", "^base=0x(%x+) len=(%d+) pid=(%d+)$")
  if not base then error("victim handshake failed: " .. tostring(line)) end
  return {
    pipe = pipe,
    base = tonumber(base, 16),
    len = tonumber(len),
    pid = tonumber(pid),
    counter = function()
      local f = assert(io.open(counter_path, "rb"))
      local bytes = f:read(8)
      f:close()
      local v = 0
      for i = 8, 1, -1 do v = v * 256 + string.byte(bytes, i) end
      return v
    end,
    close = function(self)
      os.execute("kill -9 " .. self.pid .. " 2>/dev/null")
      self.pipe:close()
    end,
  }
end

local function sleep_ms(ms)
  os.execute(string.format("sleep %f", ms / 1000))
end

-- ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
-- (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
-- PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
local function check_rc(rc, what)
  if rc == df.PTRACE_ETRACE then
    print("# " .. what .. ": ptrace refused (ETRACE) — the lane needs " ..
          "--cap-add=SYS_PTRACE; this is NOT a valid skip")
  end
  check(rc == df.PTRACE_OK, what)
end

if not live_expected then
  print("# SKIP live-attach: not Linux/x64 (the tier is Linux x86-64 only)")
elseif not victim_exe then
  -- The lane always exports this; missing means a misconfigured lane, and silently
  -- skipping every live test is the hole this suite must not have.
  print("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-lua-test`")
  os.exit(1)
else
  -- Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub).
  check(df.live_attach_available(), "live: tier is real on Linux/x64 (EINVAL, not ENOSYS)")

  local vic = spawn_victim("1", 7, 5)
  local vt = df.ValueTrace(64, 512)
  local rc, result = vt:attach_pid(vic.pid, vic.base, vic.len)
  check_rc(rc, "live: attach_pid a FOREIGN running pid + stepped the region")
  -- The region really executed IN the victim: rax = rdi + rsi.
  check(result == 12, "live: attach_pid region returned 12 (got " .. result .. ")")
  -- Exactly df_chain's six in-region instructions — not "some".
  check(vt:steps() == 6, "live: six in-region steps captured (got " .. vt:steps() .. ")")
  check(vt:recs() > 0, "live: operand records captured")
  -- SURVIVAL: we attached to a process we do not own; it must outlive the detach.
  local c0 = vic.counter()
  sleep_ms(50)
  check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)")
  -- T3 — the memory def-use edge (step1 store -> step2 load) reached from
  -- step4 through the load at step2: the by-pointer seed (T1) is what makes
  -- this slice reachable at all in this binding.
  local fwd0 = vt:forward_slice(0)
  local bwd4 = vt:backward_slice(4)
  check(list_eq(fwd0, { 0, 1, 2, 3, 4 }),
        "live: forward_slice(0) == {0,1,2,3,4} over df_chain, excludes ret")
  check(list_eq(bwd4, { 0, 1, 2, 3, 4 }),
        "live: backward_slice(4) == {0,1,2,3,4} -- the memory edge step1(store)->step2(load), " ..
        "excludes ret")
  vt:free()
  vic:close()

  -- THE anti-hardcode control: a second victim, different args, same wrapper.
  vic = spawn_victim("2", 17, 25)
  vt = df.ValueTrace(64, 512)
  rc, result = vt:attach_pid(vic.pid, vic.base, vic.len)
  check_rc(rc, "live: attach_pid the second victim")
  check(result == 42, "live: result TRACKS the victim's args (17+25=42, got " .. result .. ")")
  check(vt:steps() == 6, "live: six steps on the second victim too")
  vt:free()
  vic:close()

  vic = spawn_victim("3", 9, 4)
  vt = df.ValueTrace(64, 512)
  -- only_tid 0: step whichever thread enters the region (here, the only one).
  rc, result = vt:attach_pid_tid(vic.pid, 0, vic.base, vic.len)
  check_rc(rc, "live: attach_pid_tid stepped the entering thread")
  check(result == 13, "live: attach_pid_tid region returned 13 (got " .. result .. ")")
  check(vt:steps() == 6, "live: attach_pid_tid captured six steps")
  vt:free()
  vic:close()

  vic = spawn_victim("4", 20, 3)
  vt = df.ValueTrace(64, 512)
  local survived
  rc, result, survived = vt:attach_jit(vic.pid, 0, vic.base, vic.len)
  check_rc(rc, "live: attach_jit stepped the region")
  check(result == 23, "live: attach_jit region returned 23 (got " .. result .. ")")
  check(vt:steps() == 6, "live: attach_jit captured six steps")
  -- The producer's OWN survival report — the house rule that a foreign target is
  -- never killed, asserted from its side.
  check(survived == 1, "live: attach_jit reported the target as survived")
  c0 = vic.counter()
  sleep_ms(50)
  check(vic.counter() > c0, "live: attach_jit victim kept running after detach")
  vt:free()
  vic:close()

  -- T4 — a real code-image threaded through attach_pid_versioned: build the
  -- recorder over the victim's OWN published region, then decode the capture
  -- against it. A non-NULL img must not break the capture or land in the
  -- wrong argument slot (a dropped/misplaced pointer would corrupt base/pid
  -- and the result assert below would catch it).
  if df.codeimage_available() then
    vic = spawn_victim("5", 11, 6)
    local img = df.CodeImage(vic.pid)
    local trc = img:track(vic.base, vic.len)
    check(trc == df.CI_OK, "codeimage: track() over the victim's published region")
    vt = df.ValueTrace(64, 512)
    rc, result = vt:attach_pid_versioned(vic.pid, vic.base, vic.len, img, img:now())
    check_rc(rc, "live: attach_pid_versioned with a real img")
    check(result == 17,
          "live: attach_pid_versioned result TRACKS the victim's args (11+6=17, got " ..
          result .. ")")
    check(vt:steps() == 6, "live: attach_pid_versioned captured six steps with a real img")
    img:free()
    vt:free()
    vic:close()
  else
    print("# SKIP codeimage live: " .. df.codeimage_skip_reason())
  end

  -- Negative control: the wrapper must surface the producer's rejections rather
  -- than manufacture success.
  vt = df.ValueTrace(8, 8)
  check(select(1, vt:attach_pid(12345, 0x1000, 0)) == df.PTRACE_EINVAL,
        "live: zero-length region is rejected (EINVAL)")
  check(select(1, vt:attach_pid(0, 0x1000, 21)) == df.PTRACE_EINVAL,
        "live: pid 0 is rejected (EINVAL)")
  check(select(1, vt:attach_pid(0x7FFFFFF0, 0x1000, 21)) ~= df.PTRACE_OK,
        "live: attaching to a nonexistent pid never returns OK")
  vt:free()
end

print("1.." .. n)
os.exit(failed and 1 or 0)
