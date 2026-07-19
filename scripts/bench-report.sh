#!/bin/sh
# bench-report.sh — the cross-system benchmark runner (Phase 3).
#
# Runs the three producers on THIS system and merges them, with an OS-portable
# system descriptor, into one normalized per-system report:
#   - native BENCH tier   real cyc/ticks per call (host arch)       test_bench
#   - emu-bench           deterministic insn/block counts per ISA   emu-bench
#   - asmfeatures         capability + trace-completeness sweep      asmfeatures
#
#   scripts/bench-report.sh            write build/bench-report-<os>-<arch>.json
#   scripts/bench-report.sh --record   also persist into the benchmarks/ tree
#
# POSIX shell for the descriptor probe (Linux/macOS/Windows-git-bash differ only
# here); the JSON merge + persistence is done by python3 (present on every CI leg)
# so the output is always well-formed. See
# docs/internal/plans/cross-arch-benchmarking-plan.md.
set -eu

BUILD="${BUILD:-build}"
RECORD=0
[ "${1:-}" = "--record" ] && RECORD=1

# --- system descriptor (OS-portable) ---------------------------------------
os_raw="$(uname -s 2>/dev/null || echo unknown)"
arch_raw="$(uname -m 2>/dev/null || echo unknown)"
case "$arch_raw" in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) arch="$arch_raw" ;;
esac

cpu="unknown"
vendor="unknown"
os=unknown
os_version="$os_raw"
case "$os_raw" in
    Linux)
        os=linux
        cpu="$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
        [ -z "$cpu" ] && cpu="$(sed -n 's/^Model[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
        vendor="$(sed -n 's/^vendor_id[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
        if [ -r /etc/os-release ]; then
            os_version="$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-linux}") / $(uname -r 2>/dev/null)"
        else
            os_version="$(uname -sr 2>/dev/null)"
        fi
        ;;
    Darwin)
        os=macos
        cpu="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
        vendor="$(sysctl -n machdep.cpu.vendor 2>/dev/null || echo Apple)"
        os_version="$(sw_vers -productName 2>/dev/null || echo macOS) $(sw_vers -productVersion 2>/dev/null || true)"
        ;;
    MINGW* | MSYS* | CYGWIN* | Windows*)
        os=windows
        cpu="${PROCESSOR_IDENTIFIER:-unknown}"
        vendor="${PROCESSOR_IDENTIFIER:-unknown}"
        os_version="$os_raw"
        ;;
    *) os="$(printf '%s' "$os_raw" | tr 'A-Z' 'a-z')" ;;
esac
[ -z "$cpu" ] && cpu="unknown"
[ -z "$vendor" ] && vendor="unknown"

# short vendor slug for the box id
case "$vendor" in
    *AuthenticAMD* | *AMD*) vendor_short=amd ;;
    *GenuineIntel* | *Intel*) vendor_short=intel ;;
    *Apple*) vendor_short=apple ;;
    *ARM* | *arm*) vendor_short=arm ;;
    *) vendor_short="$(printf '%s' "$vendor" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9' | cut -c1-8)" ;;
esac
[ -z "$vendor_short" ] && vendor_short=cpu

# stable per-machine key: <vendor>-<os>-<arch>-<hash8 of the cpu model>
hash8="$(printf '%s|%s|%s' "$cpu" "$os" "$arch" | { sha1sum 2>/dev/null || shasum 2>/dev/null || md5 2>/dev/null || cksum; } | cut -c1-8)"
box_id="${ASMTEST_BOX_ID:-${vendor_short}-${os}-${arch}-${hash8}}"

uarch="${ASMTEST_UARCH:-unknown}"
# Best-effort microarchitecture label from the CPU family (Linux). Honest
# "unknown" when we cannot tell; the env override always wins.
if [ "$uarch" = unknown ] && [ "$os" = linux ]; then
    fam="$(sed -n 's/^cpu family[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
    if [ "$vendor_short" = amd ]; then
        case "$fam" in
            23) uarch=zen1-2 ;;
            25) uarch=zen3-4 ;;
            26) uarch=zen5 ;;
        esac
    fi
fi
virtualized=false
[ "${ASMTEST_VIRTUALIZED:-0}" = "1" ] && virtualized=true
cc_ver="$(${CC:-cc} --version 2>/dev/null | head -1 || echo unknown)"
version="$(cat VERSION 2>/dev/null || echo unknown)"
commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
ts="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"

# --- run the producers into fragment files ---------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$BUILD/test_bench" --bench --bench-format=json --bench-reps="${BENCH_REPS:-4096}" \
    >"$tmp/native.json" 2>/dev/null
"$BUILD/emu-bench" --format=json >"$tmp/emu.json"
"$BUILD/asmfeatures" >"$tmp/features.json"

out="$BUILD/bench-report-${os}-${arch}.json"

# --- merge + (optionally) persist, via python3 -----------------------------
BOX_ID="$box_id" OS="$os" OS_VERSION="$os_version" ARCH="$arch" CPU="$cpu" \
UARCH="$uarch" VENDOR="$vendor" CC_VER="$cc_ver" VIRT="$virtualized" \
VERSION="$version" COMMIT="$commit" TS="$ts" TMP="$tmp" OUT="$out" \
RECORD="$RECORD" python3 - <<'PY'
import json, os, sys, pathlib

tmp = os.environ["TMP"]
def load(name):
    with open(os.path.join(tmp, name)) as f:
        return json.load(f)

native = load("native.json")
emu = load("emu.json")
features = load("features.json")["features"]

system = {
    "box_id": os.environ["BOX_ID"],
    "os": os.environ["OS"],
    "os_version": os.environ["OS_VERSION"],
    "arch": os.environ["ARCH"],
    "cpu": os.environ["CPU"],
    "uarch": os.environ["UARCH"],
    "vendor": os.environ["VENDOR"],
    "cc": os.environ["CC_VER"],
    "virtualized": os.environ["VIRT"] == "true",
    "asmtest_version": os.environ["VERSION"],
    "commit": os.environ["COMMIT"],
    "timestamp": os.environ["TS"],
}
report = {
    "schema": "asmtest-bench-report/v1",
    "system": system,
    "performance": {"native": native, "emulated": emu},
    "features": features,
}
out = os.environ["OUT"]
with open(out, "w") as f:
    json.dump(report, f, indent=2, sort_keys=False)
    f.write("\n")

# ---- short human summary to stderr ----
def w(s): sys.stderr.write(s + "\n")
w("bench-report: %s" % out)
w("  system : %s  (%s, %s)" % (system["box_id"], system["cpu"], system["os_version"]))
w("  native : %s per call, %d cases%s" % (
    native.get("unit", "?"), len(native.get("benchmarks", [])),
    "  [virtualized: not comparable]" if system["virtualized"] else ""))
w("  emu    : %d deterministic count rows" % len(emu.get("benchmarks", [])))
avail = sum(1 for x in features if x.get("available"))
comp = [x for x in features if x.get("complete") is not None]
compok = sum(1 for x in comp if x.get("complete"))
w("  feature: %d/%d capabilities available; trace completeness %d/%d complete" % (
    avail, len(features), compok, len(comp)))

if os.environ["RECORD"] != "1":
    sys.exit(0)

# ---- persistence (Phase 5): golden emu counts + per-box history + features --
root = pathlib.Path("benchmarks")
gold = root / "golden"
boxdir = root / "boxes" / system["box_id"]
gold.mkdir(parents=True, exist_ok=True)
boxdir.mkdir(parents=True, exist_ok=True)

# Golden: the deterministic emu-bench counts — host/OS-independent, so this one
# file is the SAME on every leg and any change is a reviewable, gated diff. Only
# `insns` rows go in: `model_cost` rows are Capstone-version-dependent (repo pin
# 5.0.1 vs MSYS2 5.0.9), so they are NOT "identical on every leg" material and are
# filtered here to match bench-golden-check.py's read-side filter.
golden = {"schema": "asmtest-emu-golden/v1",
          "benchmarks": sorted(
              (r for r in emu.get("benchmarks", [])
               if r.get("kind", "insns") == "insns"),
              key=lambda r: (r["name"], r["arch"]))}
with open(gold / "emu-insns.json", "w") as f:
    json.dump(golden, f, indent=2)
    f.write("\n")

# Per-box capability record (feature set + descriptor; a function of the box/OS).
with open(boxdir / "features.json", "w") as f:
    json.dump({"system": system, "features": features}, f, indent=2)
    f.write("\n")

# Per-box real-cycle history: one append-only line per run (trend, not assertion).
line = {"timestamp": system["timestamp"], "commit": system["commit"],
        "os": system["os"], "arch": system["arch"], "unit": native.get("unit"),
        "virtualized": system["virtualized"],
        "native": [{"name": b["name"], "median": b["median"], "unit": native.get("unit")}
                   for b in native.get("benchmarks", [])]}
with open(boxdir / "perf-history.jsonl", "a") as f:
    f.write(json.dumps(line) + "\n")

w("bench-record: persisted golden emu-insns.json + boxes/%s/{features.json,perf-history.jsonl}" % system["box_id"])
PY

# echo the report to stdout (so `make docker-bench` shows it even with --rm)
cat "$out"
