#!/bin/sh
# runner-jit-loop.sh — standing ephemeral self-hosted runner via per-job JIT
# configs: the production loop docs/internal/ci/runners.md prescribes for the
# unattended nightly hw.yml lanes.
#
# An ephemeral/JIT runner de-registers after exactly ONE job, so a compromised
# job cannot persist on the runner identity. To keep a box available for
# scheduled runs, loop: mint a fresh JIT config, run one job, repeat. Wrap this
# script in a systemd unit (Linux) or launchd plist (macOS) that restarts on
# exit — the unit template lives in the runbook. That pairing IS the ephemeral
# loop GitHub recommends (autoscale only with ephemeral, never persistent,
# runners); never `./svc.sh install` a persistent service instead.
#
# Usage: runner-jit-loop.sh <owner/repo> <lane-label> <runner-dir>
#   e.g. runner-jit-loop.sh wilvk/asm-test intel-pt "$HOME/actions-runner-intel-pt"
#
# Requirements (runbook: Registration flow + Security posture):
#   - <runner-dir> holds the PINNED runner release, its tarball SHA-256 already
#     verified ON THIS BOX against GitHub's published digest and recorded in the
#     runbook status table. This script never downloads runner software.
#   - `gh` authenticated with repo-admin on <owner/repo> (the JIT mint needs
#     it). That is a standing credential on the box — a posture tradeoff the
#     runbook records; hw.yml's no-PR/owner-actor guards are what bound it.
#   - The lane's HW_RUNNER_* repo variable stays `1` while this loop runs, and
#     goes back to `0` when the box powers down (operator rule), so the nightly
#     skips instead of failing at a pickup timeout.
#
# Stopping the loop while a runner is idle can leave a stale runner record;
# GitHub reaps ephemeral runners that stay offline, so no manual cleanup is
# needed beyond resetting the HW_RUNNER_* variable.
set -eu

usage() { echo "usage: $0 <owner/repo> <lane-label> <runner-dir>" >&2; exit 2; }
[ $# -eq 3 ] || usage
repo=$1
lane=$2
dir=$3

log() { echo "runner-jit-loop: $*" >&2; }

[ -x "$dir/run.sh" ] || { log "no runner software at $dir (see the runbook's pinned-download flow)"; exit 1; }
command -v gh >/dev/null 2>&1 || { log "gh not found on PATH"; exit 1; }

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) log "unsupported OS $(uname -s)"; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64) arch=x64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) log "unsupported arch $(uname -m)"; exit 1 ;;
esac

host=$(hostname -s 2>/dev/null || echo box)
child=
i=0
trap 'log "stopping (signal)"; [ -n "$child" ] && kill "$child" 2>/dev/null; exit 0' INT TERM

while :; do
    i=$((i + 1))
    name="${lane}-${host}-$$-${i}"
    log "minting JIT config for $name (labels: self-hosted $os $arch $lane)"
    if ! jit=$(gh api -X POST "/repos/$repo/actions/runners/generate-jitconfig" \
        -f name="$name" -F runner_group_id=1 \
        -f 'labels[]=self-hosted' -f "labels[]=$os" \
        -f "labels[]=$arch" -f "labels[]=$lane" \
        -q .encoded_jit_config); then
        log "JIT mint failed (network? token?) — retrying in 60s"
        sleep 60
        continue
    fi
    log "listening as $name (one job, then re-register)"
    "$dir/run.sh" --jitconfig "$jit" &
    child=$!
    rc=0
    wait "$child" || rc=$?
    child=
    log "runner exited rc=$rc; re-registering in 5s"
    sleep 5
done
