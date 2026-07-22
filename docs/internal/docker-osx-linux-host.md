# Setting up the Docker-OSX clean-room lane on a Linux host

Operator runbook for bringing **Track D** (`make docker-osx-bindings`, the
vanilla x86-64 macOS clean room) up on a new bare-metal Linux box. It captures
what the first real shakedown attempt (2026-07-22/23, Ryzen 9 4900HS, Ubuntu,
snap Docker) actually hit, so the next host takes an hour of babysitting
instead of an afternoon of debugging. That attempt ended **blocked, not
green**: the host's boot carried a kernel-measured TSC warp, which freezes
macOS/KVM guests under load no matter what (see the clocksource check below —
it is a hard gate, verify it FIRST). The lane itself is documented for users
in [docs/clean-room-testing.md](../clean-room-testing.md); the task-level spec
is
[implementations/macos-cleanroom-lanes.md](implementations/macos-cleanroom-lanes.md).

The flow has two halves: a **one-time interactive install** that produces a
reusable macOS disk image, then the **repeatable headless lane** that boots
that disk and runs the clean-room install test. Only the first half is manual.

## Host prerequisites

- Bare-metal Linux with KVM: `/dev/kvm` must exist and be writable by your
  user (`ls -la /dev/kvm`; an ACL entry or `kvm` group membership both work).
  Hosted CI runners expose no nested KVM — this lane is self-hosted-only, and
  EULA-gray on non-Apple hardware: opt-in, never wired into hosted CI.
- Docker able to pass the device through: verify with
  `docker run --rm --device /dev/kvm ubuntu:24.04 ls /dev/kvm`.
- ~35 GB free disk: the installed guest disk settles around 20–25 GB (sparse
  qcow2), plus the ~4 GB wrapper image and a ~1 GB recovery download.
- ~5 GB of genuinely free RAM — the guest runs at the image's 4 GB default.
- **Check the host clocksource FIRST — it is a hard gate** (ignoring it cost
  this shakedown its whole evening):

  ```sh
  cat /sys/devices/system/clocksource/clocksource0/current_clocksource
  ```

  If it prints anything but `tsc`, **stop: this boot cannot run the lane.**
  The kernel measured real TSC trouble, and macOS guests — whose only
  timebase is the TSC — freeze under sustained load on such a boot. On the
  shakedown host the journal shows it happened **at boot** from inter-core
  desync (`Measured 4680 cycles TSC warp between CPUs, turning off TSC
  clock`, `Marking TSC unstable due to check_tsc_sync_source failed`);
  `journalctl -k | grep -i tsc` names the reason on any host.

  Everything tried on that warped boot froze, only at different points —
  treat these as measured dead ends, not options:
  - `SMP=4` (image default): livelock at late guest boot, every vCPU
    spinning at a fixed kernel RIP (`kvm: SMP vm created on host with
    unstable TSC` in the kernel log).
  - `SMP=1`: install froze ~35 min in, mid-download.
  - `SMP=1` + QEMU pinned to one physical core (SMT sibling pair,
    `--cpuset-cpus` — sound in theory: siblings share their core's TSC):
    froze ~15 min in.
  - `SMP=1` + pin + CPUID `tsc-deadline` masked (legacy APIC timer): froze
    ~5 min in.

  The freeze signature is always the same: guest UI pixel-frozen (not even
  spinners animate), zero guest disk and network I/O, the QEMU vCPU thread
  burning 100% of a hw thread split roughly half user / half kernel (a KVM
  exit storm), QEMU main loop idle. **The fix is a host reboot** — TSC sync
  is re-measured at boot; verify `current_clocksource` reads `tsc`
  afterwards, then proceed (plain `SMP=4` is fine on a healthy-TSC host).

  On a healthy-but-borderline host, the lane script still warns and pins
  QEMU to cpu0's SMT sibling pair when it sees a non-tsc clocksource
  (`DOCKER_OSX_CPUSET` overrides; empty disables) — belt-and-braces, but do
  not expect the pin to rescue a boot the kernel already flagged: it did
  not, live.

### Snap-packaged Docker quirks (Ubuntu)

- `docker stop`/`docker kill` on the QEMU container can fail with
  `permission denied` (AppArmor signal mediation between the snap profile and
  host shells). Work around it from inside the same confinement:
  `docker exec <container> kill <qemu-pid-in-container>`.
- Bind mounts from `/tmp/...` silently mount empty. Keep the disk image and
  any mounts under `$HOME` or the repo tree (this runbook uses
  `build/osx/`, which is gitignored).

## One-time install (produces the reusable disk)

The upstream prebuilt-disk images died with the 2024 Docker Hub deletion, so
a fresh install is unavoidable. Everything below is drivable over VNC — no X11
on the host, no graphical session needed.

1. **Create a host-backed sparse disk** (avoids a tens-of-GB `docker cp`
   at the end — the qcow2 lives on the host from the start; the image ships
   `qemu-img` so nothing is installed on the host):

   ```sh
   mkdir -p build/osx
   docker run --rm -v "$PWD/build/osx:/out" --entrypoint qemu-img \
     sickcodes/docker-osx:latest create -f qcow2 /out/mac_hdd_ng.img 80G
   ```

2. **Launch the installer container** — VNC display instead of X11, disk
   mounted over `IMAGE_PATH`, and the CPU/SMP settings that actually boot:

   ```sh
   docker run -di --name asmtest-osx-install --device /dev/kvm \
     -p 50922:10022 -p 5999:5999 \
     -v "$PWD/build/osx/mac_hdd_ng.img:/image" -e IMAGE_PATH=/image \
     -e GENERATE_UNIQUE=true \
     -e CPU=Haswell-noTSX-IBRS \
     -e SHORTNAME=ventura \
     -e EXTRA="-display none -vnc 0.0.0.0:99" \
     sickcodes/docker-osx:latest
   ```

   Why these flags — each one was a real failure first:
   - `-di` (not just `-d`): the image's `Launch.sh` runs QEMU with
     `-monitor stdio`; a closed stdin EOFs the monitor and QEMU quits.
   - `EXTRA=-display none -vnc …`: QEMU's default gtk display is fatal in an
     X-less container. VNC lands on host port 5999 (display 99).
   - `CPU=Haswell-noTSX-IBRS`: the image's `Penryn` default predates features
     newer macOS userlands assume — the Sequoia recovery crash-loop-spins on
     it. Haswell-level is the well-trodden macOS-KVM profile, and any
     x86-64-v3 host (Zen 2+, Haswell+) can back it under KVM.
   - `SHORTNAME=ventura`: picks which recovery the first boot downloads.
     Ventura installs comfortably inside the 4 GB guest-RAM default; Sequoia
     wants more RAM than a loaded 16 GB host can spare. Any macOS ≥ the
     bindings' floor works for the clean room — Ventura is also what this
     lane originally targeted.
   - Do **not** attempt any of this while `current_clocksource` is not
     `tsc` — that is the hard gate above; no SMP/pin combination survived
     it live.
   - First boot downloads the ~850 MB recovery from Apple's CDN and
     generates unique serials before QEMU appears (several minutes;
     `docker logs -f asmtest-osx-install`).
   - Optional iteration speedup: once the recovery download has happened,
     `docker commit <container> <local-cache-image>` snapshots it; container
     recreations from that image skip the re-download (the serial generator
     still re-runs). Purely a local cache — the disk itself lives on the
     host either way.

3. **Drive the install over VNC** (any client; scriptably:
   `pip install vncdotool`, then `vncdo -s 127.0.0.1::5999 capture s.png`,
   `key enter`, `move X Y click 1`, `type text`). Two vncdotool gotchas,
   both hit live:
   - `type` silently drops Shift for shifted characters (`&&` arrives as
     `77`, `*` as `8`). Send those as explicit key events (`key shift-8`
     for `*`) or phrase commands to avoid them — the lane's all-lowercase
     `user`/`alpine` credentials type cleanly.
   - The OpenCore picker does **not** wait forever: after its timeout it
     auto-boots the default (first) entry, which is the Base System while
     the InstallMedia is attached. Harmless before the install starts; at
     the post-phase-1 reboots you have to catch the picker and select
     **macOS Installer** yourself (see below).

   The flow:
   - OpenCore picker → boot **macOS Base System** (Enter).
   - Recovery (≈3–5 min to UI at 1 vCPU) → **Disk Utility** → select the
     ~86 GB uninitialized *QEMU HARDDISK Media* (NOT the 400 MB OpenCore
     disk, NOT the ~3 GB Base System) → **Erase** as APFS/GUID, name e.g.
     `macos` → quit Disk Utility.
   - **Reinstall macOS Ventura** → Agree ×2 → select the erased disk →
     install. This is a recovery-based install: phase 1 *downloads the full
     ~11 GB of Ventura from Apple's CDN* (that is what its "About 2 hours
     remaining" mostly is, through QEMU user-net at 1 vCPU). Budget 2–4 h
     total; the guest reboots several times. At each reboot, catch the
     picker and boot **macOS Installer** (the target disk's entry) until it
     becomes plain `macos`.
   - Transient recovery hiccups seen live: **"Failed to download required
     installer asset. (UpdateBrain.zip)"** — dismiss, relaunch the
     installer, retry (it proceeded on the second attempt; check the guest
     `date` in Terminal only if it repeats — TLS needs a sane clock). After
     any interrupted phase-1 attempt, delete the leftover download state
     from Utilities → Terminal before retrying:
     `rm -rf /Volumes/macos/"macOS Install Data"` (type it
     shift-safely: `rm -rf /volumes/macos/mac` + `key shift-8`).
   - Setup Assistant: region → skip everything optional (Migration, Apple
     ID, Siri, Screen Time, Location) → **create the account
     `user` / `alpine`** (the lane's default credentials; anything else
     needs `ASMTEST_OSX_USER`/`ASMTEST_OSX_PASS` on every run) → at the
     desktop, open Terminal and turn on Remote Login:

     ```
     sudo systemsetup -setremotelogin on    # password: alpine
     ```

   - Shut down cleanly (` → Shut Down`), then remove the container:
     `docker rm -f asmtest-osx-install`. The finished disk is
     `build/osx/mac_hdd_ng.img` on the host — no `docker cp` needed.

## The repeatable headless lane

```sh
DOCKER_OSX_DISK=$PWD/build/osx/mac_hdd_ng.img \
DOCKER_OSX_CPU=Haswell-noTSX-IBRS \
make docker-osx-bindings
```

(`current_clocksource` must read `tsc` — the hard gate above applies to the
lane exactly as it does to the install; the script warns and pins if not.)

The lane stages nothing by itself — the host must have run
`gh run download <release-run> -n native-all -D build/dist/native` (or an
Intel-mac build) plus `make packages`/the per-language packers first, exactly
as [clean-room-testing.md](../clean-room-testing.md) describes; the script
preflights the darwin-x86_64 payload and refuses to boot without it.
`DOCKER_OSX_VNC=99` re-attaches a VNC display for triage. The script's sshd
wait loop allows 30 minutes; a prebuilt-disk boot is expected well inside
that (not yet measured — the shakedown never got a completed disk).

## Troubleshooting quick table

| Symptom | Cause | Fix |
|---|---|---|
| QEMU exits immediately, `gtk initialization failed` in logs | no display in container | the lane script now passes `-display none`; for manual runs set `EXTRA` as above |
| Container exits as soon as it detaches | `-monitor stdio` EOF | run with `-di`, not `-d` |
| Verbose boot freezes early, vCPUs pegged, on `CPU=Penryn` | macOS userland needs post-Penryn ISA | `CPU=Haswell-noTSX-IBRS` |
| Boot freezes at late launchd, all vCPUs at fixed kernel RIPs, any CPU model | host TSC demoted (`current_clocksource` ≠ `tsc`) | hard gate: reboot the host until clocksource is `tsc` |
| Guest UI freezes (even spinners stop), **zero** disk + net I/O, vCPU 100% ~half user/half kernel | same warped-TSC boot — no SMP/pin/`-tsc-deadline` combination survives it | reboot the host; then delete `macOS Install Data` and redo the install |
| "Failed to download required installer asset. (UpdateBrain.zip)" | transient CDN fetch in recovery | dismiss, relaunch installer, retry; if persistent check guest `date` |
| `docker stop`/`kill` → `permission denied` | snap AppArmor signal mediation | `docker exec <c> kill <pid>` |
| Bind-mounted file is empty in the container | snap Docker `/tmp` restriction | keep mounts under `$HOME`/repo |
| Guest sshd never up on a virgin image | no prebuilt disk — it booted the installer | do the one-time install; set `DOCKER_OSX_DISK` |
| `-cpu max,la57=off` in `pgrep` output | that's the serial-generator's libguestfs appliance, not your guest | ignore |
