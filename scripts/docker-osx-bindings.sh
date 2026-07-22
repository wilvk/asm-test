#!/bin/sh
# docker-osx-bindings.sh — Track D of docs/internal/plans/macos-clean-test-plan.md: an
# on-demand x86-64 macOS clean room on a bare-metal Linux host, without waiting
# on the scarce nightly macos-13 runner. Driven by Docker-OSX
# (https://github.com/sickcodes/Docker-OSX — QEMU + OpenCore + OVMF + a macOS
# recovery image): boot the guest headless with KVM, SSH in on localhost:50922,
# copy the working tree — with the HOST-staged packages — in, and run the SAME
# Track-A clean-room install test (scripts/clean-room-test.sh under
# scripts/clean-env.sh + scripts/assert-clean-path.sh), then tear the container
# down. Driven by `make docker-osx-bindings`.
#
# *** VALIDATED END TO END 2026-07-23 (first green shakedown): Ryzen 9 9950X
# *** (Zen 5), healthy TSC (current_clocksource=tsc). The one-time Ventura 13.7.8
# *** install completed headless over VNC -> build/osx/mac_hdd_ng.img (user/alpine,
# *** Remote Login on), and `DOCKER_OSX_DISK=... DOCKER_OSX_CPU=Haswell-noTSX-IBRS
# *** make docker-osx-bindings` exited rc=0 (stable x2): clean-room-test OK on
# *** darwin-x86_64, ruby PASS (bundled dylib) / rest SKIP. The 2026-07-22/23
# *** attempt (Ryzen 9 4900HS, snap Docker) that hardened this script (DOCKER_OSX_*
# *** knobs, `-display none`, `-di`, the tar exclude) blocked only because THAT
# *** host's BOOT had a warped TSC (clocksource demoted to hpet) — macOS/KVM guests
# *** freeze under load on such a boot, so the host's clocksource MUST read tsc.
# *** Full evidence + one-time-install runbook: docs/internal/docker-osx-linux-host.md.
#
# Honest tradeoffs (per the plan): x86-only (the OpenCore path has no arm64
# guest); a "virtualized Hackintosh" that can break on macOS point-updates;
# EULA-gray on non-Apple hosts; a tens-of-GB image; software-rendered graphics
# (irrelevant for this headless CLI smoke). NOT a duplicate of the Rosetta CI
# leg — that proves the x86 ABI under Rosetta on Apple Silicon; this proves a
# clean-room x86 dlopen on a vanilla Intel-macOS userland.
#
# As in Track C the guest is treated as toolchain-free: stage the packages on
# the host first (a darwin-x86_64 payload comes from an Intel-mac build or a
# release `native-all` artifact — a Linux host cannot build Mach-O payloads),
# and the guest runs clean-room-test.sh with ASMTEST_CLEANROOM_PREBUILT=1.
#
# Requirements (host side): bare-metal Linux with /dev/kvm (hosted CI runners
# do not expose nested KVM — the make target hard-errors there by design) and
# docker. sshpass runs containerized (`make docker-sshpass`, Dockerfile.sshpass)
# — no host sshpass/sudo needed. First boot of a fresh image can take tens of
# minutes.
#
# Image tags (verified 2026-07-17): sickcodes/docker-osx's Docker Hub repo was
# emptied ~2024-08-28 and only `latest`/`master` were re-pushed since — every
# other tag (:ventura, :auto, :sonoma, :sequoia, :naked, ...) 404s. `:latest`'s
# CMD downloads the Sequoia recovery/installer on first boot — a fresh install
# needing one interactive session — so a HEADLESS run needs a prebuilt disk via
# DOCKER_OSX_DISK (below); without it this script prints a warning and the boot
# will very likely time out waiting for sshd.
#
# One-time interactive install (produces the reusable disk DOCKER_OSX_DISK
# points at — budget hours and tens of GB free; do this once per host):
#   docker run -it --name asmtest-osx-install --device /dev/kvm -p 50922:10022 \
#     -v /tmp/.X11-unix:/tmp/.X11-unix -e "DISPLAY=${DISPLAY:-:0.0}" \
#     -e GENERATE_UNIQUE=true sickcodes/docker-osx:latest
#   # In the QEMU window: boot the recovery, Disk Utility -> erase the largest
#   # virtio disk (APFS), install macOS, create the account `user`/`alpine`
#   # (matches this script's defaults — anything else needs ASMTEST_OSX_USER/
#   # ASMTEST_OSX_PASS), then System Settings -> General -> Sharing -> Remote
#   # Login ON. Shut the guest down cleanly, then:
#   docker cp asmtest-osx-install:/home/arch/OSX-KVM/mac_hdd_ng.img ./mac_hdd_ng.img
#   docker rm -f asmtest-osx-install
#   # (if that path is absent: `docker exec asmtest-osx-install find / -name
#   # 'mac_hdd_ng*.img'`). The mount convention below (`-v <disk>:/image -e
#   # IMAGE_PATH=/image`) is the long-standing Docker-OSX reuse flow; whether
#   # the current :latest Launch.sh honors an overridden IMAGE_PATH is
#   # shakedown-verified on the first real headless run, not pre-verified here
#   # — if it is not honored, build Dockerfile.naked from the upstream repo at
#   # a recorded commit as a fallback and note that commit here.
#
# Usage: scripts/docker-osx-bindings.sh    (or `make docker-osx-bindings`)
#   DOCKER_OSX_IMAGE   guest image     (default: sickcodes/docker-osx:latest)
#   DOCKER_OSX_DISK    prebuilt mac_hdd_ng.img from the one-time install above
#                      (unset: virgin :latest boots the installer, not headless)
#   DOCKER_OSX_CPU     QEMU CPU model passed to the image's Launch.sh (its
#                      default Penryn predates the features newer macOS
#                      userlands assume; Haswell-noTSX-IBRS is known good —
#                      shakedown-verified 2026-07-22 on a Zen 2 host)
#   DOCKER_OSX_SMP     guest vCPU count (default: the image's 4). Set =1 on
#                      hosts whose TSC the kernel has marked unstable
#                      (current_clocksource != tsc): macOS guests livelock on
#                      warped TSCs. On such hosts this script also pins the
#                      container to one physical core (see DOCKER_OSX_CPUSET)
#                      — SMP=1 alone proved insufficient on the shakedown host.
#   DOCKER_OSX_CPUSET  --cpuset-cpus value for the QEMU container. Default:
#                      auto — on unstable-TSC hosts, cpu0's SMT sibling pair
#                      (one physical core = one coherent TSC); unused when the
#                      host clocksource is tsc. Set empty to disable the pin.
#   DOCKER_OSX_VNC     QEMU VNC display number (e.g. 99 -> host port 5999) for
#                      watching/triaging the headless guest; unset = no VNC.
#   ASMTEST_OSX_USER / ASMTEST_OSX_PASS  guest credentials — only meaningful if
#                      that account was created during the one-time install
#                      above (Docker-OSX's historical user/alpine defaults
#                      belonged to a now-dead prebuilt image, not :latest)
#   ASMTEST_OSX_BOOT_TRIES  sshd wait attempts, 10 s apart (default: 180)
set -eu

prog=$(basename "$0")
REPO=${ASMTEST_REPO_ROOT:-$(pwd)}
REPO=$(cd "$REPO" && pwd -P)
IMG=${DOCKER_OSX_IMAGE:-sickcodes/docker-osx:latest}
DISK=${DOCKER_OSX_DISK:-}
GUSER=${ASMTEST_OSX_USER:-user}
GPASS=${ASMTEST_OSX_PASS:-alpine}
TRIES=${ASMTEST_OSX_BOOT_TRIES:-180}
NAME=asmtest-docker-osx
SSHPASS_IMG=${ASMTEST_SSHPASS_IMAGE:-asmtest-sshpass}

# HAVE_KVM guard (also enforced by the make target, with a friendlier message).
[ -e /dev/kvm ] || {
    echo "$prog: /dev/kvm absent — this lane needs bare-metal Linux with KVM" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "$prog: docker not found" >&2; exit 1; }
docker image inspect "$SSHPASS_IMG" >/dev/null 2>&1 || {
    echo "$prog: sshpass image '$SSHPASS_IMG' not built — run 'make docker-sshpass'" >&2; exit 1; }

# The guest installs prebuilt packages only; it needs the darwin-x86_64 slot.
[ -f "$REPO/build/dist/native/darwin-x86_64/libasmtest_emu.dylib" ] || {
    echo "$prog: no staged darwin-x86_64 payload under build/dist/native/ —" >&2
    echo "$prog: fetch a release 'native-all' artifact (or build on an Intel mac), then 'make packages'" >&2
    exit 1
}

# A demoted host TSC (current_clocksource != tsc) means the kernel measured
# inter-core TSC warp. macOS guests have a TSC-only timebase and livelock once
# their vCPU lands on a warped core — at ANY vCPU count: the first shakedown
# host (Ryzen 9 4900HS, boot-time "Measured 4680 cycles TSC warp between
# CPUs") wedged mid-install even at SMP=1 after ~35 min. Confine QEMU to one
# physical core — SMT siblings read the same core TSC, so the guest clock
# stays coherent. Slower, but it survives. DOCKER_OSX_CPUSET overrides the
# derived sibling pair (set it empty to disable the pin entirely).
CLKSRC_F=/sys/devices/system/clocksource/clocksource0/current_clocksource
CPUSET=
if [ -r "$CLKSRC_F" ] && [ "$(cat "$CLKSRC_F")" != tsc ]; then
    CPUSET=$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list 2>/dev/null || echo 0)
    [ -n "${DOCKER_OSX_CPUSET+set}" ] && CPUSET=$DOCKER_OSX_CPUSET
    echo "$prog: WARNING host clocksource is '$(cat "$CLKSRC_F")', not tsc (inter-core TSC warp) —" >&2
    echo "$prog: macOS guests livelock on warped TSCs; pinning QEMU to cpuset '${CPUSET:-<disabled>}'" >&2
    if [ "${DOCKER_OSX_SMP:-4}" != 1 ]; then
        echo "$prog: set DOCKER_OSX_SMP=1 too — extra vCPUs on one pinned core only add risk" >&2
    fi
fi

echo "$prog: booting $IMG headless (SSH forwarded to localhost:50922)"
docker rm -f "$NAME" >/dev/null 2>&1 || true
# -i keeps the container's stdin open: the image's Launch.sh runs QEMU with
# `-monitor stdio`, and a closed stdin EOFs the monitor, which quits QEMU.
# EXTRA drops QEMU's default display (gtk — fatal in an X-less container) in
# favor of none/VNC; both were found on the first real headless boot.
set -- -di --name "$NAME" --device /dev/kvm -p 50922:10022 \
    -e NOPICKER=true -e GENERATE_UNIQUE=true \
    -e "EXTRA=-display none${DOCKER_OSX_VNC:+ -vnc 0.0.0.0:$DOCKER_OSX_VNC}"
[ -n "${DOCKER_OSX_VNC:-}" ] && set -- "$@" -p "$((5900 + DOCKER_OSX_VNC)):$((5900 + DOCKER_OSX_VNC))"
[ -n "${DOCKER_OSX_CPU:-}" ] && set -- "$@" -e "CPU=$DOCKER_OSX_CPU"
[ -n "${DOCKER_OSX_SMP:-}" ] && set -- "$@" -e "SMP=$DOCKER_OSX_SMP" -e "CORES=${DOCKER_OSX_CORES:-$DOCKER_OSX_SMP}"
[ -n "$CPUSET" ] && set -- "$@" --cpuset-cpus "$CPUSET"
if [ -n "$DISK" ]; then
    [ -f "$DISK" ] || { echo "$prog: DOCKER_OSX_DISK '$DISK' not found" >&2; exit 1; }
    set -- "$@" -v "$DISK:/image" -e IMAGE_PATH=/image
else
    echo "$prog: WARNING no DOCKER_OSX_DISK — a virgin $IMG boots the macOS *installer*," >&2
    echo "$prog: which needs one interactive session first; headless sshd will likely time out." >&2
fi
docker run "$@" "$IMG" >/dev/null
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT INT TERM

ssh_g() {
    docker run --rm -i --network host -e SSHPASS="$GPASS" "$SSHPASS_IMG" \
        sshpass -e ssh -p 50922 \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR "$GUSER@localhost" "$@"
}

# First boot runs the macOS installer; be patient, but fail loudly in the end.
echo "$prog: waiting for sshd in the guest (up to $((TRIES * 10 / 60)) min)"
i=0
until ssh_g true 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge "$TRIES" ]; then
        echo "$prog: guest sshd never came up; container logs follow" >&2
        docker logs --tail 50 "$NAME" >&2 || true
        exit 1
    fi
    sleep 10
done
echo "$prog: guest up"

echo "$prog: copying the working tree (incl. staged build/dist) into the guest"
# Never ship the guest its own live disk image: the operator runbook stages
# the qcow2 inside the repo (build/osx/), and tar-ing a multi-GB image QEMU
# is actively writing would both balloon the copy and race the writer.
set -- --exclude .git
case "$DISK" in
    "$REPO"/*) set -- "$@" --exclude ".${DISK#"$REPO"}" ;;
esac
tar -czf - -C "$REPO" "$@" . \
    | ssh_g 'rm -rf asmtest && mkdir asmtest && tar -xzf - -C asmtest'

echo "$prog: running the Track-A clean-room install test in the x86 macOS guest"
rc=0
ssh_g 'cd asmtest && ASMTEST_CLEANROOM_PREBUILT=1 ASMTEST_REPO_ROOT="$PWD" sh scripts/clean-room-test.sh' || rc=$?

echo "$prog: done (rc=$rc); removing the container"
exit $rc
