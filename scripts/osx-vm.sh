#!/bin/sh
# osx-vm.sh — Track C of docs/internal/plans/macos-clean-test-plan.md: the highest-
# fidelity arm64 macOS clean room. Clone a VANILLA macOS VM image (no Xcode, no
# Homebrew) with tart (Apple's Virtualization.framework, near-bare-metal), boot
# it headless, copy the working tree — with the HOST-staged packages — into the
# guest, run the Track-A clean-room install test (scripts/clean-room-test.sh:
# fresh install + scripts/clean-env.sh scrub + scripts/assert-clean-path.sh
# resolved-path assertion) over SSH, then delete the VM. Cloning from the base
# gives a byte-identical room every run.
#
# *** WRITTEN PER THE PLAN, NOT YET VALIDATED. This was authored in an
# *** environment with no Apple-Silicon host (tart is Apple-Silicon-only), so
# *** the script has never been executed end to end. Treat the first run as a
# *** shakedown and update the plan's Track C status when it goes green.
#
# The guest is toolchain-free ON PURPOSE — a binding that only works because the
# hosted runner is "dirty" (Xcode CLT, Homebrew) surfaces here. Consequences:
#   - packaging cannot run inside the guest; stage everything on the host first
#     (`make packages package-libs`), and the guest runs clean-room-test.sh with
#     ASMTEST_CLEANROOM_PREBUILT=1 (install + smoke + assert only);
#   - bindings whose runtime is absent in a vanilla image self-skip — that is
#     the expected shape of a green run, not a failure.
#
# EULA note: macOS's license permits up to 2 macOS VMs on APPLE hardware, so
# tart-on-Mac is above board (unlike Track D's Docker-OSX on non-Apple hosts).
#
# Requirements (host side, Apple Silicon only):
#   - tart:    brew install cirruslabs/cli/tart
#   - sshpass: the vanilla images use password auth (admin/admin)
#   - a vanilla base image, e.g.:
#       tart clone ghcr.io/cirruslabs/macos-sequoia-vanilla:15.7.7 macos-vanilla
#
# Usage: scripts/osx-vm.sh    (or `make osx-vm-test`)
#   ASMTEST_TART_BASE  base VM/image name             (default: macos-vanilla)
#   ASMTEST_TART_VM    name of the throwaway clone    (default: asmtest-cleanroom)
#   ASMTEST_TART_USER / ASMTEST_TART_PASS  guest credentials (default: admin/admin)
set -eu

prog=$(basename "$0")
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
BASE=${ASMTEST_TART_BASE:-macos-vanilla}
VM=${ASMTEST_TART_VM:-asmtest-cleanroom}
GUSER=${ASMTEST_TART_USER:-admin}
GPASS=${ASMTEST_TART_PASS:-admin}

# Guards: this lane only exists on an Apple-Silicon macOS host with tart.
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo "$prog: Apple-Silicon macOS only (host is $(uname -s)/$(uname -m)) — see docs/clean-room-testing.md" >&2
    exit 1
fi
command -v tart >/dev/null 2>&1 || {
    echo "$prog: tart not found — brew install cirruslabs/cli/tart" >&2; exit 1; }
command -v sshpass >/dev/null 2>&1 || {
    echo "$prog: sshpass not found (vanilla images use password auth) — brew install sshpass" >&2; exit 1; }
tart get "$BASE" >/dev/null 2>&1 || {
    echo "$prog: no base VM '$BASE' — e.g.: tart clone ghcr.io/cirruslabs/macos-sequoia-vanilla:15.7.7 $BASE" >&2; exit 1; }

# The guest is toolchain-free, so the payload + packages must be staged HERE.
[ -f "$REPO/build/dist/native/darwin-arm64/libasmtest_emu.dylib" ] || {
    echo "$prog: no staged darwin-arm64 payload — run 'make packages package-libs' first (the vanilla guest cannot build them)" >&2
    exit 1
}

echo "$prog: cloning $BASE -> $VM"
tart clone "$BASE" "$VM"
cleanup() {
    tart stop "$VM" >/dev/null 2>&1 || true
    tart delete "$VM" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

echo "$prog: booting $VM headless"
tart run --no-graphics "$VM" >/dev/null 2>&1 &

# Wait for DHCP, then for sshd.
ip=""
i=0
while [ $i -lt 60 ]; do
    ip=$(tart ip "$VM" 2>/dev/null || true)
    [ -n "$ip" ] && break
    sleep 2; i=$((i + 1))
done
[ -n "$ip" ] || { echo "$prog: VM never reported an IP" >&2; exit 1; }

ssh_g() {
    sshpass -p "$GPASS" ssh -p 22 \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR "$GUSER@$ip" "$@"
}

i=0
until ssh_g true 2>/dev/null; do
    i=$((i + 1))
    [ $i -lt 60 ] || { echo "$prog: sshd in the guest never came up ($ip)" >&2; exit 1; }
    sleep 2
done
echo "$prog: guest up at $ip"

echo "$prog: copying the working tree (incl. staged build/dist) into the guest"
tar -czf - -C "$REPO" --exclude .git . \
    | ssh_g 'rm -rf asmtest && mkdir asmtest && tar -xzf - -C asmtest'

echo "$prog: running the Track-A clean-room install test in the vanilla guest"
rc=0
ssh_g 'cd asmtest && ASMTEST_CLEANROOM_PREBUILT=1 ASMTEST_REPO_ROOT="$PWD" sh scripts/clean-room-test.sh' || rc=$?

echo "$prog: done (rc=$rc); deleting the VM"
exit $rc
