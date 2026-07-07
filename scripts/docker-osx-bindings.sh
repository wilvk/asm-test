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
# *** WRITTEN PER THE PLAN, NOT YET VALIDATED. This was authored in an
# *** environment without /dev/kvm on bare metal, so the script has never been
# *** executed end to end. Treat the first run as a shakedown and update the
# *** plan's Track D status when it goes green.
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
# do not expose nested KVM — the make target hard-errors there by design),
# docker, sshpass. First boot of a fresh image can take tens of minutes.
#
# Usage: scripts/docker-osx-bindings.sh    (or `make docker-osx-bindings`)
#   DOCKER_OSX_IMAGE   guest image     (default: sickcodes/docker-osx:ventura)
#   ASMTEST_OSX_USER / ASMTEST_OSX_PASS  guest credentials (Docker-OSX default: user/alpine)
#   ASMTEST_OSX_BOOT_TRIES  sshd wait attempts, 10 s apart (default: 180)
set -eu

prog=$(basename "$0")
REPO=${ASMTEST_REPO_ROOT:-$(pwd)}
REPO=$(cd "$REPO" && pwd -P)
IMG=${DOCKER_OSX_IMAGE:-sickcodes/docker-osx:ventura}
GUSER=${ASMTEST_OSX_USER:-user}
GPASS=${ASMTEST_OSX_PASS:-alpine}
TRIES=${ASMTEST_OSX_BOOT_TRIES:-180}
NAME=asmtest-docker-osx

# HAVE_KVM guard (also enforced by the make target, with a friendlier message).
[ -e /dev/kvm ] || {
    echo "$prog: /dev/kvm absent — this lane needs bare-metal Linux with KVM" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "$prog: docker not found" >&2; exit 1; }
command -v sshpass >/dev/null 2>&1 || {
    echo "$prog: sshpass not found (the guest uses password auth) — apt-get install sshpass" >&2; exit 1; }

# The guest installs prebuilt packages only; it needs the darwin-x86_64 slot.
[ -f "$REPO/build/dist/native/darwin-x86_64/libasmtest_emu.dylib" ] || {
    echo "$prog: no staged darwin-x86_64 payload under build/dist/native/ —" >&2
    echo "$prog: fetch a release 'native-all' artifact (or build on an Intel mac), then 'make packages'" >&2
    exit 1
}

echo "$prog: booting $IMG headless (SSH forwarded to localhost:50922)"
docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" \
    --device /dev/kvm \
    -p 50922:10022 \
    -e NOPICKER=true \
    -e GENERATE_UNIQUE=true \
    "$IMG" >/dev/null
trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT INT TERM

ssh_g() {
    sshpass -p "$GPASS" ssh -p 50922 \
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
tar -czf - -C "$REPO" --exclude .git . \
    | ssh_g 'rm -rf asmtest && mkdir asmtest && tar -xzf - -C asmtest'

echo "$prog: running the Track-A clean-room install test in the x86 macOS guest"
rc=0
ssh_g 'cd asmtest && ASMTEST_CLEANROOM_PREBUILT=1 ASMTEST_REPO_ROOT="$PWD" sh scripts/clean-room-test.sh' || rc=$?

echo "$prog: done (rc=$rc); removing the container"
exit $rc
